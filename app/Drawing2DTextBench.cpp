// Benches of drawing text and pens (UI-92): text shaped as a text renderer shapes it, outlined on the load worker (core
// drawing_text.cpp), and lines in linetypes and lineweights of their own. Cases in tools/bench_cases/drawing2d.py; the
// shaping, bidi order and layout alone are tests/test_drawing_text, the DXF side tests/test_dxf, line styles test_drawing2d.
#include <QCoreApplication>
#include <QImage>

#include <Bnd_Box.hxx>
#include <TopExp_Explorer.hxx>

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
// other; "III" in a shape font (.shx) beside the drawing is three strokes, lines without fills; MTEXT formatted part by
// part shows its red part in red, its fraction's parts above the capitals and below the baseline over each other, and
// its underline as one run of ink under the word; "I." in a right-to-left override is drawn ".I" (its leftmost ink is
// the dot, low, not the stroke). <prefix>.png.
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
                      guide = layerNamed(scene, "Guide"), mixed = layerNamed(scene, "Mixed"), shape = layerNamed(scene, "Shape");
    require(shown && !joined.empty() && !latin.empty() && !right.empty() && !guide.empty() && !mixed.empty() && !shape.empty(),
            QString("the text layers are shown: %1 bodies").arg(v->displayedCount()));
    if (!shown || joined.empty() || latin.empty() || right.empty() || guide.empty() || mixed.empty() || shape.empty()) return QCoreApplication::exit(2);
    auto box = [&w](const std::string& layer) {
      Bnd_Box b;
      for (const auto& body : w.m_doc->scene.bodies_under(layer)) b.Add(opad::node_world_bbox(w.m_doc->doc, w.m_doc->scene, body));
      return b;
    };
    v->fitAll();
    pollUntil(&w, [] { return true; }, 300, [&w, v, require, all, value, box, joined, latin, right, guide, mixed, shape](bool) {
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
      // The shape font's 'III': three strokes (lines, no fills) drawn as three runs of ink in every row.
      int faces = 0, edges = 0;
      for (const auto& id : w.m_doc->scene.bodies_under(shape)) {
        const TopoDS_Shape s = opad::node_world_shape(w.m_doc->doc, w.m_doc->scene, id);
        for (TopExp_Explorer e(s, TopAbs_FACE); e.More(); e.Next()) ++faces;
        for (TopExp_Explorer e(s, TopAbs_EDGE); e.More(); e.Next()) ++edges;
      }
      const int shapeRuns = fewestRuns(pixels(shape));
      require(faces == 0 && edges == 3 && shapeRuns == 3, QString("'III' in a shape font beside the drawing is its strokes (%1 lines, %2 fills, %3 runs of ink)").arg(edges).arg(faces).arg(shapeRuns));
      // MTEXT in parts: Rich "Plain {\C1;Red} 1\S1/2;" (capitals 5 high), Ruled "\LUnder\l"; the drawing
      // is centred, so the red part's box says where the baseline lies (it is "Red": on the baseline, as high as capitals).
      const opad::Scene& scene = w.m_doc->scene;
      const std::string rich = layerNamed(scene, "Rich"), ruled = layerNamed(scene, "Ruled");
      std::string redPart;
      for (const auto& id : rich.empty() ? std::vector<std::string>{} : scene.bodies_under(rich))
        if (const auto* n = scene.node(id); n->has_color && n->color[0] > 0.99 && n->color[1] < 0.01 && n->color[2] < 0.01) redPart = id;
      require(!rich.empty() && !ruled.empty() && !redPart.empty(), "MTEXT's red part is a body of its own on its layer");
      if (rich.empty() || ruled.empty() || redPart.empty()) return QCoreApplication::exit(2);
      auto area = [v](double x0, double y0, double x1, double y1) {  // world rectangle -> image pixels
        const QPoint p = v->widgetPoint({x0, y0, 0}), q = v->widgetPoint({x1, y1, 0});
        const double s = v->displayScale();
        return QRect(QPoint(qRound(std::min(p.x(), q.x()) * s), qRound(std::min(p.y(), q.y()) * s)),
                     QPoint(qRound(std::max(p.x(), q.x()) * s), qRound(std::max(p.y(), q.y()) * s)));
      };
      auto inked = [&image, ink](const QRect& r, int& left, int& right) {  // ink pixels in r, and how far they reach
        int n = 0;
        left = 1 << 30, right = -1;
        for (int y = std::max(0, r.top()); y <= std::min(image.height() - 1, r.bottom()); ++y)
          for (int x = std::max(0, r.left()); x <= std::min(image.width() - 1, r.right()); ++x)
            if (ink(x, y)) ++n, left = std::min(left, x), right = std::max(right, x);
        return n;
      };
      double x0, y0, z0, x1, y1, z1;
      opad::node_world_bbox(w.m_doc->doc, scene, redPart).Get(x0, y0, z0, x1, y1, z1);
      const QRect redBox = area(x0, y0, x1, y1);
      int reds = 0;
      for (int y = std::max(0, redBox.top()); y <= std::min(image.height() - 1, redBox.bottom()); ++y)
        for (int x = std::max(0, redBox.left()); x <= std::min(image.width() - 1, redBox.right()); ++x) {
          const QColor c = image.pixelColor(x, y);
          reds += c.red() > 150 && c.green() < 90 && c.blue() < 90;
        }
      require(reds > 10, QString("the red part is drawn red (%1 red pixels)").arg(reds));
      const double baseline = y0;
      box(rich).Get(x0, y0, z0, x1, y1, z1);
      int upLeft, upRight, downLeft, downRight;
      const int up = inked(area(x0, baseline + 5.4, x1, baseline + 6.4), upLeft, upRight), down = inked(area(x0, baseline - 1.6, x1, baseline - 0.4), downLeft, downRight);
      require(up > 0 && down > 0 && upLeft <= downRight && downLeft <= upRight,
              QString("the fraction stacks: ink above the capitals (%1 px at %2..%3) over ink below the baseline (%4 px at %5..%6)")
                  .arg(up).arg(upLeft).arg(upRight).arg(down).arg(downLeft).arg(downRight));
      box(ruled).Get(x0, y0, z0, x1, y1, z1);  // the underline is its lowest edge
      const QRect under = area(x0, y0 - 0.5, x1, y0 + 0.5);
      int longest = 0;
      for (int y = std::max(0, under.top()); y <= std::min(image.height() - 1, under.bottom()); ++y)
        for (int x = std::max(0, under.left() - 2), run = 0; x <= std::min(image.width() - 1, under.right() + 2); ++x)
          longest = std::max(longest, run = ink(x, y) ? run + 1 : 0);
      require(longest >= 0.8 * under.width(), QString("the underline is one run of ink under its word (%1 of %2 px)").arg(longest).arg(under.width()));
      // An explicit override (RLO "I." PDF): the leftmost column of ink is the dot's, short, not the full-height stroke's.
      const std::string override = layerNamed(scene, "Override");
      const QRect turned = override.empty() ? QRect() : pixels(override);
      int column = -1, inkTop = image.height(), inkBottom = -1;
      for (int x = std::max(0, turned.left() - 2); column < 0 && x <= std::min(image.width() - 1, turned.right() + 2); ++x)
        for (int y = std::max(0, turned.top()); y <= std::min(image.height() - 1, turned.bottom()); ++y)
          if (ink(x, y)) column = x, inkTop = std::min(inkTop, y), inkBottom = std::max(inkBottom, y);
      require(!override.empty() && column >= 0 && inkBottom - inkTop < 0.4 * turned.height(),
              QString("a right-to-left override draws \"I.\" as \".I\" (its leftmost ink %1 of %2 px high)").arg(inkBottom - inkTop + 1).arg(turned.height()));
      QCoreApplication::exit(*all ? 0 : 2);
    });
  });
  return true;
}

// OPAD_BENCH_PENS=<prefix> on a DXF whose Walls layer is dashed and 0.5 mm wide (tools/bench_cases/drawing2d.py
// pens_file): a line on it in a linetype of its own (CENTER) is drawn in its own dashes and the layer's width, one with a
// lineweight of its own (1.00 mm) in the layer's dashes and its own width, the layer's other lines in the layer's; a block
// line by block takes its insert's (HIDDEN on Plain), one with a linetype scale of its own (0.5) in the layer's dashes at
// half their size. The layer turned HIDDEN and 0.35 mm in one step: its own lines keep what is theirs, the scaled one
// takes HIDDEN at half size. <prefix>.png
OPAD_BENCH(OPAD_BENCH_PENS, pens) {
  auto all = std::make_shared<bool>(true);
  Check require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: pens: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    *all = *all && ok;
  };
  Viewport* v = w.m_viewport;
  auto settled = [&w, v] {
    int expected = 0;
    for (const auto& id : w.m_doc->scene.all_bodies()) expected += w.m_doc->scene.effectively_visible(id);
    return !w.m_displayJob && w.m_meshRemaining == 0 && v->displayedCount() >= expected && expected > 0 && !v->looksPending();
  };
  pollUntil(&w, settled, 60000, [&w, v, require, all, value, settled](bool shown) {
    // The body on a layer whose own line style (Node::line) has these fields (null: none), "" none.
    auto body = [&w](const std::string& layer, const opad::json& line) {
      for (const auto& id : w.m_doc->scene.bodies_under(layerNamed(w.m_doc->scene, layer))) {
        const opad::json& own = w.m_doc->scene.node(id)->line;
        bool match = line.is_null() ? own.is_null() : own.is_object();
        if (line.is_object())
          for (const auto& [key, field] : line.items()) match = match && own.value(key, opad::json()) == field;
        if (match) return id;
      }
      return std::string();
    };
    auto drawnAs = [v](const std::string& id, const char* linetype, double mm, double scale = 1) {
      const opad::json state = v->benchLookState(id);
      const drawing2d::LinePattern p = drawing2d::linePattern(drawing2d::dashes(linetype), drawing2d::kPatternPixelsPerMm * scale * v->displayScale() * v->renderScale());
      return state.is_object() && state.value("linePattern", 0) == p.bits && state.value("lineFactor", 0) == p.factor &&
             state.value("lineWidth", 0.0) == v->lineWidth(drawing2d::linePoints(mm));
    };
    const std::string byLayer = body("Walls", opad::json()), center = body("Walls", {{"linetype", "CENTER"}}),
                      heavy = body("Walls", {{"lineweight", 1.0}}), byBlock = body("Plain", {{"linetype", "HIDDEN"}}),
                      scaled = body("Walls", {{"scale", 0.5}});
    require(shown && !byLayer.empty() && !center.empty() && !heavy.empty() && !byBlock.empty() && !scaled.empty(),
            QString("the drawing is shown, its lines with styles of their own in bodies of their own (%1 bodies)").arg(v->displayedCount()));
    if (!shown || byLayer.empty() || center.empty() || heavy.empty() || byBlock.empty() || scaled.empty()) return QCoreApplication::exit(2);
    require(drawnAs(byLayer, "DASHED", 0.5) && drawnAs(center, "CENTER", 0.5) && drawnAs(heavy, "DASHED", 1.0) && drawnAs(byBlock, "HIDDEN", -1),
            "the layer's lines dashed and 0.5 mm, CENTER of its own, 1.00 mm of its own, the block line in its insert's HIDDEN");
    auto dash = [v](const std::string& id) {  // the longest dash the view draws, in pixels: on bits in a row times the factor
      const opad::json state = v->benchLookState(id);
      const int bits = state.value("linePattern", 0xFFFF), factor = state.value("lineFactor", 1);
      int longest = 0;
      for (int start = 0; start < 16; ++start) {
        int n = 0;
        while (n < 16 && (bits >> ((start + n) % 16) & 1)) ++n;
        longest = std::max(longest, n);
      }
      return longest * factor;
    };
    require(drawnAs(scaled, "DASHED", 0.5, 0.5) && dash(scaled) > 0 && dash(scaled) <= 0.65 * dash(byLayer),
            QString("the line with a linetype scale of its own (0.5) in the layer's dashes at half their size (%1 px against %2)").arg(dash(scaled)).arg(dash(byLayer)));
    v->fitAll();
    const drawing2d::Layer walls = *drawing2d::layerAt(w.m_doc->scene, byLayer);
    w.m_doc->runAll({{"appearance", drawing2d::setLinetype(walls, "HIDDEN")}, {"appearance", drawing2d::setLineweight(walls, 0.35)}}, "layer pens");
    pollUntil(&w, [v, drawnAs, byLayer] { return !v->looksPending() && drawnAs(byLayer, "HIDDEN", 0.35); }, 10000,
              [&w, v, require, all, value, drawnAs, byLayer, center, heavy, scaled](bool changed) {
      require(changed && drawnAs(center, "CENTER", 0.35) && drawnAs(heavy, "HIDDEN", 1.0) && drawnAs(scaled, "HIDDEN", 0.35, 0.5),
              "the layer turned HIDDEN and 0.35 mm: its lines follow (the scaled one at half size), the CENTER line keeps its dashes and the heavy one its width");
      v->grabImage().save(value + ".png");
      QCoreApplication::exit(*all ? 0 : 2);
    });
  });
  return true;
}
