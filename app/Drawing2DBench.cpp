// Benches of the 2D drawing area (drawing2d): contrast of drawings without a colour on every background (UI-10). Cases in
// tools/bench_cases/drawing2d.py; the colour rules alone are tests/test_drawing2d.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

#include "BenchRegistry.hpp"
#include "Drawing2D.hpp"
#include "MainWindow.hpp"
#include "Theme.hpp"
#include "opad/geometry.hpp"

namespace {
// Polls `done` every 50 ms until it holds or `ms` have passed, then calls `then` with the outcome.
void pollUntil(QObject* context, std::function<bool()> done, int ms, std::function<void(bool)> then) {
  auto* timer = new QTimer(context);
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  QObject::connect(timer, &QTimer::timeout, context, [timer, clock, done, ms, then] {
    const bool ok = done();
    if (!ok && clock->elapsed() < ms) return;
    timer->stop();
    timer->deleteLater();
    then(ok);
  });
  timer->start(50);
}

using Check = std::function<void(bool, const QString&)>;
drawing2d::Rgb rgb(const QColor& c) { return {c.redF(), c.greenF(), c.blueF()}; }

// The layer component of this name (the drawing's layers are components holding its bodies), "" if none.
std::string layerNamed(const opad::Scene& scene, const std::string& name) {
  for (const auto& [id, n] : scene.nodes)
    if (n.kind == opad::Node::Kind::Component && n.name == name) return id;
  return {};
}
}  // namespace

// OPAD_BENCH_CONTRAST=<prefix>: a DXF drawn in colour 7 (lines, a fill, a fill with a line in one body) and in red, viewed
// in both themes on each scene background (theme, gradient, white, dark). Every frame: the fill is drawn in the ink of the
// background (light on dark, dark on light) and stands out from it by at least 4.5:1, so do the lines (the brightest or
// darkest pixel across them, antialiased), the red line keeps its colour, lines are as wide as the display scale.
// <prefix>.<dark|light>.<background>.png.
OPAD_BENCH(OPAD_BENCH_CONTRAST, contrast) {
  auto all = std::make_shared<bool>(true);
  Check require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: contrast: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    *all = *all && ok;
  };
  Viewport* v = w.m_viewport;
  auto settled = [&w, v] {
    int expected = 0;
    for (const auto& id : w.m_doc->scene.all_bodies()) expected += w.m_doc->scene.effectively_visible(id);
    return !w.m_displayJob && w.m_meshRemaining == 0 && v->displayedCount() >= expected && expected > 0 && !v->looksPending();
  };
  pollUntil(&w, settled, 60000, [&w, v, require, all, value](bool shown) {
    const opad::Scene& scene = w.m_doc->scene;
    const std::string fill = layerNamed(scene, "Fill"), lines = layerNamed(scene, "Lines"), mixed = layerNamed(scene, "Mixed"), red = layerNamed(scene, "Red");
    require(shown && !fill.empty() && !lines.empty() && !mixed.empty() && !red.empty(), QString("the drawing is shown: %1 bodies").arg(v->displayedCount()));
    if (!shown || fill.empty() || lines.empty() || mixed.empty() || red.empty()) return QCoreApplication::exit(2);
    // Image pixels of world points: the fill's centre, the middle of the colour-7 line and of the mixed layer's line.
    auto at = [&w, v](const std::string& layer, double fy) {
      Bnd_Box box;
      for (const auto& body : w.m_doc->scene.bodies_under(layer)) box.Add(opad::node_world_bbox(w.m_doc->doc, w.m_doc->scene, body));
      double x0, y0, z0, x1, y1, z1;
      box.Get(x0, y0, z0, x1, y1, z1);
      const QPoint p = v->widgetPoint({(x0 + x1) / 2, y0 + (y1 - y0) * fy, (z0 + z1) / 2});
      return QPoint(qRound(p.x() * v->displayScale()), qRound(p.y() * v->displayScale()));
    };
    struct Frame {
      bool dark;
      int background;
    };
    auto frames = std::make_shared<std::vector<Frame>>();
    for (bool dark : {true, false})
      for (int background : {0, 1, 2, 3}) frames->push_back({dark, background});
    auto step = std::make_shared<std::function<void(size_t)>>();
    *step = [&w, v, require, all, value, frames, step, at, fill, lines, mixed, red](size_t i) {
      if (i == frames->size()) return QCoreApplication::exit(*all ? 0 : 2);
      const Frame f = (*frames)[i];
      w.applyTheme(f.dark);
      v->setSceneBackground(f.background);
      pollUntil(&w, [v] { return !v->looksPending(); }, 10000, [&w, v, require, value, step, i, f, at, fill, lines, mixed, red](bool applied) {
        static const char* names[] = {"theme", "gradient", "white", "dark"};
        const QString frame = QString("%1 theme, %2 background").arg(f.dark ? "dark" : "light", names[f.background]);
        const QImage image = v->grabImage();
        image.save(QString("%1.%2.%3.png").arg(value, f.dark ? "dark" : "light", names[f.background]));
        const QPoint centre = at(fill, 0.5);
        const QColor background = image.pixelColor(3, centre.y()), filled = image.pixelColor(centre);
        const auto ink = v->drawingInk();
        const double fillContrast = drawing2d::contrast(rgb(filled), rgb(background));
        const bool inked = std::abs(filled.redF() - ink[0]) < 3 / 255.0 && std::abs(filled.greenF() - ink[1]) < 3 / 255.0;
        require(applied && inked && fillContrast >= 4.5,
                QString("%1: the colour-7 fill is drawn in the ink %2 on %3, contrast %4:1").arg(frame, filled.name(), background.name()).arg(fillContrast, 0, 'f', 1));
        // Across a line: the pixel that stands out most within 4 pixels of its middle.
        auto lineContrast = [&image, background](const QPoint& p) {
          double best = 1;
          for (int dy = -4; dy <= 4; ++dy)
            if (image.rect().contains(p.x(), p.y() + dy)) best = std::max(best, drawing2d::contrast(rgb(image.pixelColor(p.x(), p.y() + dy)), rgb(background)));
          return best;
        };
        const double line = lineContrast(at(lines, 0.5)), free = lineContrast(at(mixed, 1.0));
        require(line >= 4.5 && free >= 4.5, QString("%1: colour-7 hairlines stand out %2:1, a line beside a fill in its body %3:1").arg(frame).arg(line, 0, 'f', 1).arg(free, 0, 'f', 1));
        const auto redBodies = w.m_doc->scene.bodies_under(red), lineBodies = w.m_doc->scene.bodies_under(lines);
        const BodyLook redLook = v->shownLook(redBodies.at(0));
        const opad::json state = v->benchLookState(lineBodies.at(0));
        const double width = state.value("lineWidth", 0.0), least = v->displayScale() * v->renderScale();
        require(std::abs(redLook.color[0] - 1) < 1e-6 && redLook.color[1] < 1e-6 && width >= least - 1e-6 && width < least + 1 && width == std::round(width),
                QString("%1: the red line keeps its colour, lines are %2 px wide (display scale %3, render scale %4)").arg(frame).arg(width).arg(v->displayScale()).arg(v->renderScale()));
        (*step)(i + 1);
      });
    };
    v->fitAll();
    (*step)(0);
  });
  return true;
}
