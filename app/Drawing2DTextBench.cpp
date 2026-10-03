// Benches of drawing text (UI-92): text shaped as a text renderer shapes it, outlined on the load worker (core
// drawing_text.cpp). Cases in tools/bench_cases/drawing2d.py; the shaping, bidi order and layout alone are
// tests/test_drawing_text, the DXF placement tests/test_dxf.
#include <QCoreApplication>
#include <QImage>

#include <Bnd_Box.hxx>

#include <algorithm>
#include <cmath>
#include <memory>

#include "BenchRegistry.hpp"
#include "Drawing2D.hpp"
#include "Drawing2DBench.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "opad/geometry.hpp"

using namespace bench2d;

// OPAD_BENCH_TEXT2D=<prefix> on a DXF with Arabic, Latin and mixed text (tools/bench_cases/drawing2d.py text_file): the
// text layers are shown; in the view, three behs written in a row are one joined word (some row of pixels across it is one
// run of ink: the letters take their joining forms and touch) while "III" stays three strokes in every row; the Arabic
// word right-aligned on the guide line ends at it; Latin and Arabic in one line lie side by side, not on top of each
// other. <prefix>.png.
OPAD_BENCH(OPAD_BENCH_TEXT2D, text2d) {
  auto all = std::make_shared<bool>(true);
  Check require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: text2d: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
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
    const std::string joined = layerNamed(scene, "Joined"), latin = layerNamed(scene, "Latin"), right = layerNamed(scene, "Right"),
                      guide = layerNamed(scene, "Guide"), mixed = layerNamed(scene, "Mixed");
    require(shown && !joined.empty() && !latin.empty() && !right.empty() && !guide.empty() && !mixed.empty(),
            QString("the text layers are shown: %1 bodies").arg(v->displayedCount()));
    if (!shown || joined.empty() || latin.empty() || right.empty() || guide.empty() || mixed.empty()) return QCoreApplication::exit(2);
    auto box = [&w](const std::string& layer) {
      Bnd_Box b;
      for (const auto& body : w.m_doc->scene.bodies_under(layer)) b.Add(opad::node_world_bbox(w.m_doc->doc, w.m_doc->scene, body));
      return b;
    };
    v->fitAll();
    pollUntil(&w, [] { return true; }, 300, [&w, v, require, all, value, box, joined, latin, right, guide, mixed](bool) {
      const QImage image = v->grabImage();
      image.save(value + ".png");
      const QColor background = image.pixelColor(3, 3);
      auto ink = [&image, background](int x, int y) {
        const QColor c = image.pixelColor(x, y);
        return drawing2d::contrast({c.redF(), c.greenF(), c.blueF()}, {background.redF(), background.greenF(), background.blueF()}) > 1.6;
      };
      // The layer's box in image pixels.
      auto pixels = [v, box](const std::string& layer) {
        const Bnd_Box b = box(layer);
        double x0, y0, z0, x1, y1, z1;
        b.Get(x0, y0, z0, x1, y1, z1);
        const QPoint p = v->widgetPoint({x0, y0, 0}), q = v->widgetPoint({x1, y1, 0});
        const double s = v->displayScale();
        return QRect(QPoint(qRound(std::min(p.x(), q.x()) * s), qRound(std::min(p.y(), q.y()) * s)),
                     QPoint(qRound(std::max(p.x(), q.x()) * s), qRound(std::max(p.y(), q.y()) * s)));
      };
      // Across each row of the text's box: how many separate runs of ink; the fewest of any row that has ink.
      auto fewestRuns = [&image, ink](const QRect& r) {
        int fewest = 1000;
        for (int y = std::max(0, r.top()); y <= std::min(image.height() - 1, r.bottom()); ++y) {
          int runs = 0;
          bool in = false;
          for (int x = std::max(0, r.left() - 2); x <= std::min(image.width() - 1, r.right() + 2); ++x) {
            const bool now = ink(x, y);
            if (now && !in) ++runs;
            in = now;
          }
          if (runs > 0) fewest = std::min(fewest, runs);
        }
        return fewest;
      };
      const QRect word = pixels(joined), strokes = pixels(latin);
      const int wordRuns = fewestRuns(word), strokeRuns = fewestRuns(strokes);
      require(word.width() > 40 && wordRuns == 1, QString("three behs are one joined word (%1 px wide, %2 run(s) of ink in its most joined row)").arg(word.width()).arg(wordRuns));
      require(strokes.width() > 20 && strokeRuns == 3, QString("'III' stays three strokes in every row (%1 runs at fewest)").arg(strokeRuns));
      // Right-aligned on the guide (x = 100): the word ends at it, within a side bearing.
      const Bnd_Box r = box(right), g = box(guide);
      double rx0, ry0, rz0, rx1, ry1, rz1, gx0, gy0, gz0, gx1, gy1, gz1;
      r.Get(rx0, ry0, rz0, rx1, ry1, rz1);
      g.Get(gx0, gy0, gz0, gx1, gy1, gz1);
      require(rx1 <= gx0 + 0.01 && rx1 > gx0 - 1.5, QString("the right-aligned Arabic text ends at its guide line (%1 against %2)").arg(rx1, 0, 'f', 2).arg(gx0, 0, 'f', 2));
      // "Room غرفة": one line as wide as both words side by side (two words drawn over each other would be about half).
      const QRect line = pixels(mixed);
      require(line.width() > 3 * line.height(), QString("Latin and Arabic in one line lie side by side (%1 x %2 px)").arg(line.width()).arg(line.height()));
      QCoreApplication::exit(*all ? 0 : 2);
    });
  });
  return true;
}
