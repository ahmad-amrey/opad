// Benches of the 2D drawing area (drawing2d): contrast of drawings without a colour on every background (UI-10), the Layers
// manager (UI-89), the 2D vocabulary (UI-118). Cases in tools/bench_cases/drawing2d.py; the colour rules and the layer model alone are tests/test_drawing2d.
#include <QAction>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

#include "BenchRegistry.hpp"
#include "BrowserDelegate.hpp"
#include "BrowserPanel.hpp"
#include "Drawing2D.hpp"
#include "Drawing2DBench.hpp"
#include "I18n.hpp"
#include "LayersPanel.hpp"
#include "MainWindow.hpp"
#include "PanelFooter.hpp"
#include "Ribbon.hpp"
#include "Theme.hpp"
#include "Units.hpp"
#include "ViewportChips.hpp"
#include "opad/geometry.hpp"

using namespace bench2d;

namespace {
drawing2d::Rgb rgb(const QColor& c) { return {c.redF(), c.greenF(), c.blueF()}; }

// A left click in a tree's cell, as the mouse delivers it.
void clickCell(QTreeWidget* tree, QTreeWidgetItem* item, int column) {
  if (!item) return;
  const QRect row = tree->visualItemRect(item);
  const QPoint at(tree->header()->sectionViewportPosition(column) + tree->header()->sectionSize(column) / 2, row.center().y());
  for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
    QMouseEvent e(type, QPointF(at), QPointF(tree->viewport()->mapToGlobal(at)), Qt::LeftButton, type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                  Qt::NoModifier);
    QCoreApplication::sendEvent(tree->viewport(), &e);
  }
}
}  // namespace

// OPAD_BENCH_CONTRAST=<prefix>: a DXF drawn in colour 7 (lines, a fill, a fill with a line in one body) and in red, viewed
// in both themes on each scene background (theme, gradient, white, dark). Every frame: the fill is drawn in the ink of the
// background (light on dark, dark on light) and stands out from it by at least 4.5:1, so do the lines (the brightest or
// darkest pixel across them, antialiased), the red line keeps its colour, lines are at least one screen pixel wide.
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

// OPAD_BENCH_LAYERS=<prefix> on a DXF with a locked dashed 0.5 mm layer (Walls), one turned off and not plotted (Notes),
// a frozen one (Old) and a plain one (Plain): the Layers panel lists them as the file has them and the view draws them so
// (hidden, dashed, as wide as 0.5 mm); clicks on the On, Freeze, Lock and Plot cells change them, one step to undo each;
// colour, linetype and lineweight; isolate keeps the camera; the layer walk shows one layer at a time (an off one too) with
// its chip; layer states are saved as a view op and restored in one step, from the panel and from the Named views menu,
// and read back from the saved file (an .opad), or asked for first in viewer mode. <prefix>.panel.png, <prefix>.walk.png.
OPAD_BENCH(OPAD_BENCH_LAYERS, layers) {
  auto all = std::make_shared<bool>(true);
  Check require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: layers: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    *all = *all && ok;
  };
  Viewport* v = w.m_viewport;
  AppDocument* doc = w.m_doc;
  auto settled = [&w, v, doc] {
    int expected = 0;
    for (const auto& id : doc->scene.all_bodies()) expected += doc->scene.effectively_visible(id);
    return !w.m_displayJob && w.m_meshRemaining == 0 && v->displayedCount() >= expected && expected > 0 && !v->looksPending();
  };
  pollUntil(&w, settled, 60000, [&w, v, doc, require, all, value, settled](bool shown) {
    auto* panel = w.findChild<LayersPanel*>();
    QAction* open = w.action("drawing2d.layers");
    require(shown && panel && open && open->isEnabled(), "the drawing is shown and the Layers command is there");
    if (!shown || !panel || !open) return QCoreApplication::exit(2);
    auto layer = [panel](const std::string& name) {
      for (const auto& l : panel->layers())
        if (l.name == name) return l;
      return drawing2d::Layer{};
    };
    auto drawn = [v](const drawing2d::Layer& l) {
      const opad::json state = l.bodies.empty() ? opad::json() : v->benchLookState(l.bodies.front());  // null: not in the view
      return state.is_object() && state.value("displayed", false);
    };
    // A body drawn in a linetype's dashes: the view's stipple is the one Drawing2D makes of them at the view's scale.
    auto stippled = [v](const std::string& body, const std::string& linetype, const std::vector<double>& pattern) {
      const opad::json state = v->benchLookState(body);
      const drawing2d::LinePattern p = drawing2d::linePattern(drawing2d::dashes(linetype, pattern), drawing2d::kPatternPixelsPerMm * v->displayScale() * v->renderScale());
      return state.is_object() && state.value("linePattern", 0) == p.bits && state.value("lineFactor", 0) == p.factor;
    };
    const std::vector<double> fileDashed{12.7, -6.35, 0, -6.35};  // the file's DASHED (with a dot), sized as acad.lin's
    auto script = std::make_shared<Script>();
    auto camera = std::make_shared<opad::json>();
    auto steps = std::make_shared<int>(0);
    script->add("open", [open] { open->trigger(); }, [panel] { return panel->isVisible() && panel->layers().size() == 4; });
    script->add("as the file has them", [v, panel, layer, drawn, require, value, stippled, fileDashed] {
      const auto walls = layer("Walls"), notes = layer("Notes"), old = layer("Old"), plain = layer("Plain");
      require(walls.locked && walls.on && walls.linetype == "DASHED" && std::abs(walls.lineweight - 0.5) < 1e-9 && !notes.on && !notes.plot && old.frozen && old.on &&
                  plain.on && !plain.frozen && !plain.locked && panel->tree()->topLevelItemCount() == 4,
              "the panel lists the layers as the file has them (Walls locked, dashed, 0.5 mm; Notes off, not plotted; Old frozen)");
      const opad::json state = v->benchLookState(walls.bodies.at(0));
      require(drawn(walls) && drawn(plain) && !drawn(notes) && !drawn(old) && state.is_object() && state.value("lineWidth", 0.0) == v->lineWidth(2),
              QString("the view draws them so: off and frozen hidden, Walls %1 px wide").arg(state.value("lineWidth", 0.0)));
      require(walls.pattern == fileDashed && stippled(walls.bodies.at(0), "DASHED", fileDashed) && !stippled(walls.bodies.at(0), "DASHED", {}) &&
                  stippled(plain.bodies.at(0), "", {}) && state.value("linePattern", 0) != 0xFFFF,
              QString("Walls is drawn in the file's DASHED (dash and dot: stipple %1 x %2), not acad.lin's, Plain solid")
                  .arg(state.value("linePattern", 0), 0, 16).arg(state.value("lineFactor", 0)));
      panel->window()->grab().save(value + ".panel.png");
    });
    script->add("turn Notes on by its cell", [panel, layer] { clickCell(panel->tree(), panel->item(layer("Notes").id), LayersPanel::On); },
                [layer, drawn, settled] { return layer("Notes").on && settled() && drawn(layer("Notes")); });
    script->add("undo", [doc, require] {
      require(doc->undoLabel() == LayersPanel::tr("turn layer on"), "a click is one step to undo: " + doc->undoLabel());
      doc->undo();
    }, [layer, drawn] { return !layer("Notes").on && !drawn(layer("Notes")); });
    script->add("freeze Plain by its cell", [panel, layer] { clickCell(panel->tree(), panel->item(layer("Plain").id), LayersPanel::Freeze); },
                [layer, drawn] { return layer("Plain").frozen && !drawn(layer("Plain")); });
    script->add("the browser marks it frozen", [&w, layer, require] {
      BrowserTree* tree = w.m_browser->tree();
      auto* delegate = qobject_cast<BrowserDelegate*>(tree->itemDelegate());
      QTreeWidgetItem* row = nullptr;
      for (QTreeWidgetItemIterator it(tree); *it && !row; ++it)
        if ((*it)->data(0, browser::kIdRole).toString().toStdString() == layer("Plain").id) row = *it;
      const browser::Decoration d = row && delegate ? delegate->decoration(tree->indexFromItem(row)) : browser::Decoration();
      require(d.typeIcon == "layers" && std::any_of(d.badges.begin(), d.badges.end(), [](const browser::Badge& b) { return b.icon == "freeze" && b.clicked; }),
              "the browser shows a layer as one, a frozen one with a snowflake that opens it in the Layers panel");
    });
    script->add("thaw Plain", [panel, layer] { clickCell(panel->tree(), panel->item(layer("Plain").id), LayersPanel::Freeze); },
                [layer, drawn, settled] { return !layer("Plain").frozen && layer("Plain").on && settled() && drawn(layer("Plain")); });
    script->add("thaw Old", [panel, layer] { clickCell(panel->tree(), panel->item(layer("Old").id), LayersPanel::Freeze); },
                [layer, drawn, settled] { return !layer("Old").frozen && settled() && drawn(layer("Old")); });
    script->add("unlock Walls, plot Notes", [panel, layer] {
      clickCell(panel->tree(), panel->item(layer("Walls").id), LayersPanel::Lock);
      clickCell(panel->tree(), panel->item(layer("Notes").id), LayersPanel::Plot);
    }, [layer, doc] { return !layer("Walls").locked && !doc->scene.node(layer("Walls").id)->locked && layer("Notes").plot; });
    script->add("Plain as Center, 1 mm", [panel, layer] {
      panel->setLinetype(layer("Plain").id, "Center");
      panel->setLineweight(layer("Plain").id, 1.0);
    }, [v, layer, stippled] {
      const opad::json state = v->benchLookState(layer("Plain").bodies.at(0));
      return state.is_object() && layer("Plain").linetype == "Center" && stippled(layer("Plain").bodies.at(0), "Center", {}) && state.value("lineWidth", 0.0) == v->lineWidth(4);
    });
    script->add("Plain as Hidden", [panel, layer] { panel->setLinetype(layer("Plain").id, "Hidden"); },
                [layer, stippled] { return layer("Plain").linetype == "Hidden" && stippled(layer("Plain").bodies.at(0), "Hidden", {}); });
    script->add("Plain as the file's DASHED", [v, panel, layer, require, stippled] {
      require(!stippled(layer("Plain").bodies.at(0), "Center", {}), "Hidden is drawn in its own dashes, not Center's");
      panel->setLinetype(layer("Plain").id, "Dashed");
    }, [layer, stippled, fileDashed] { return layer("Plain").pattern == fileDashed && stippled(layer("Plain").bodies.at(0), "Dashed", fileDashed); });
    script->add("Plain back to Center", [panel, layer, require] {
      require(true, "a linetype the drawing defines brings its dashes to another layer");
      panel->setLinetype(layer("Plain").id, "Center");
    },
                [layer, stippled] { return layer("Plain").linetype == "Center" && layer("Plain").pattern.empty() && stippled(layer("Plain").bodies.at(0), "Center", {}); });
    script->add("Walls green", [panel, layer] { panel->setColor(layer("Walls").id, {0, 0.8, 0}); },
                [v, layer] { return v->shownLook(layer("Walls").bodies.at(0)).color == drawing2d::Rgb{0, 0.8, 0}; });
    script->add("Walls in its drawing colour again", [panel, layer] { panel->setDrawingColor(layer("Walls").id); },
                [v, layer] { return v->shownLook(layer("Walls").bodies.at(0)).color == drawing2d::Rgb{1, 0, 0}; });
    auto own = [layer] {  // Plain's colour-7 line: a colour of its own, not the layer's
      const auto plain = layer("Plain");
      for (const auto& b : plain.bodies)
        if (std::find(plain.byLayer.begin(), plain.byLayer.end(), b) == plain.byLayer.end()) return b;
      return std::string();
    };
    script->add("Plain red", [panel, layer] { panel->setColor(layer("Plain").id, {1, 0, 0}); },
                [v, layer] { return v->shownLook(layer("Plain").byLayer.at(0)).color == drawing2d::Rgb{1, 0, 0}; });
    script->add("its colour-7 line keeps the ink", [v, layer, own, require] {
      const auto plain = layer("Plain");
      require(plain.bodies.size() == 2 && plain.own == 1 && !own().empty() && v->shownLook(own()).color == v->drawingInk() && plain.colored && !plain.mixed,
              "a layer colour reaches the lines drawn in the layer's colour, not those in a colour of their own (DXF colour 7 keeps the ink)");
    });
    script->add("isolate Walls", [v, panel, layer, camera] {
      *camera = v->cameraJson();
      panel->isolate({layer("Walls").id});
    }, [v, layer, drawn] { return v->isIsolated() && drawn(layer("Walls")) && !drawn(layer("Plain")); });
    script->add("isolated, the camera kept", [v, camera, require] {
      require(v->cameraJson() == *camera, "isolating a layer keeps the camera");
      v->isolate({});
    }, [v, layer, drawn, settled] { return !v->isIsolated() && settled() && drawn(layer("Plain")); });
    script->add("walk", [panel] {
      panel->tree()->setCurrentItem(panel->tree()->topLevelItem(0));
      panel->startWalk();
    }, [panel, v, drawn] { return panel->walking() && v->isIsolated() && drawn(panel->layers().front()) && !drawn(panel->layers().at(1)); });
    script->add("walk on with Down", [&w, panel, v, require, value, camera] {
      QLabel* chip = nullptr;
      for (auto* label : w.m_chips->findChildren<QLabel*>())
        if (label->text() == panel->walkText()) chip = label;
      require(chip && chip->isVisibleTo(w.m_chips) && v->cameraJson() == *camera, "the walk's chip names the layer, the camera is kept: " + panel->walkText());
      v->grabImage().save(value + ".walk.png");
      QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
      QCoreApplication::sendEvent(panel->tree(), &down);
    }, [panel, drawn] { return panel->walked() == panel->layers().at(1).id && drawn(panel->layers().at(1)) && !drawn(panel->layers().at(0)); });
    script->add("the walk shows an off layer too", [panel, layer] {
      while (panel->walked() != layer("Notes").id && panel->walking()) panel->walk(1);
    }, [panel, drawn, layer] { return panel->walked() == layer("Notes").id && drawn(layer("Notes")); });
    script->add("stop the walk", [panel] { panel->stopWalk(); }, [&w, panel, v, drawn, layer, settled] {
      bool chip = false;
      for (auto* label : w.m_chips->findChildren<QLabel*>()) chip = chip || (label->isVisibleTo(w.m_chips) && label->text().contains("·") && label->text() == panel->walkText());
      return !panel->walking() && !v->isIsolated() && settled() && !drawn(layer("Notes")) && drawn(layer("Walls")) && !chip;
    });
    if (doc->browse) {
      script->add("a layer state asks to save first in viewer mode", [panel, doc, require, steps] {
        *steps = int(doc->scene.views.size());
        panel->saveState("Before");
        require(int(doc->scene.views.size()) == *steps && doc->browse, "viewer mode: a layer state is an edit, asked for first (cancelled here)");
      });
    } else {
      auto state = std::make_shared<std::string>();
      script->add("save a layer state", [panel, doc, state] {
        panel->saveState("Before");
        if (!doc->scene.views.empty()) *state = doc->scene.views.back().id;
      }, [panel, doc, state] { return !state->empty() && panel->states().size() == 1 && doc->scene.views.back().display.contains("layers"); });
      script->add("change, restore in one step", [panel, layer, state, steps, doc] {
        panel->toggle(layer("Notes").id, LayersPanel::On);
        panel->toggle(layer("Walls").id, LayersPanel::Lock);
        panel->setLinetype(layer("Walls").id, "Continuous");
        *steps = int(doc->undoLabels().size());
        panel->restoreState(*state);
      }, [layer, fileDashed] { return layer("Notes").on == false && layer("Walls").locked == false && layer("Walls").linetype == "DASHED" && layer("Walls").pattern == fileDashed; });
      script->add("one step", [doc, steps, require, layer, drawn] {
        require(int(doc->undoLabels().size()) == *steps + 1 && doc->undoLabel() == LayersPanel::tr("restore layer state") && !drawn(layer("Notes")),
                "the layer state came back in one step: " + doc->undoLabel());
      });
      script->add("restore from the Named views menu", [&w, panel, layer, state] {
        panel->toggle(layer("Old").id, LayersPanel::Freeze);
        w.restoreNamedView(*state);
      }, [layer] { return !layer("Old").frozen; });
      script->add("saved, read back", [doc, value, require] {
        doc->saveAs(value + ".opad");
        const opad::Document back = opad::Document::load(std::filesystem::path((value + ".opad").toStdU16String()));
        bool fields = false, display = false;
        for (const auto& op : back.ops) {
          fields = fields || (op.type == "appearance" && op.data.contains("layer") && op.data.contains("visible"));
          display = display || (op.type == "view" && op.data.contains("display"));
        }
        require(fields && display, "the file keeps the layer fields (with visible, for earlier builds) and the state's view");
      });
    }
    script->add("close", [panel] { panel->footer()->primary()->click(); }, [panel] { return !panel->isVisible(); });
    Script::run(&w, script, 0, require, [all] { QCoreApplication::exit(*all ? 0 : 2); });
  });
  return true;
}

// OPAD_BENCH_VOCABULARY=<prefix>. On a drawing (drawing-only, viewer mode): the Faces filter is gone (its segment, its
// action) and so are the display style and projection chips, the other filters are Groups, Objects and Points; a hovered
// line reads "Line on <layer> · 100 mm" and, after a rest, a rollover card names its type, layer, colour, linetype,
// lineweight and length; a picked one counts as an object in the status bar and Properties call it a line on its layer
// with the layer's section; with the Groups (Bodies) filter a drawing body is a group on its layer, its Properties count
// fills, objects and points. On a solid (the box fixture): all of that stays as it is until 2D mode, which takes the
// Faces filter and the chips away (the filters keep their names: no drawing), and brings them back when it ends.
// <prefix>.card.png, <prefix>.status.png, <prefix>.properties.png.
OPAD_BENCH(OPAD_BENCH_VOCABULARY, vocabulary) {
  auto all = std::make_shared<bool>(true);
  Check require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: vocabulary: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    *all = *all && ok;
  };
  Viewport* v = w.m_viewport;
  AppDocument* doc = w.m_doc;
  auto settled = [&w, v, doc] {
    int expected = 0;
    for (const auto& id : doc->scene.all_bodies()) expected += doc->scene.effectively_visible(id);
    return !w.m_displayJob && w.m_meshRemaining == 0 && v->displayedCount() >= expected && expected > 0 && !v->looksPending();
  };
  pollUntil(&w, settled, 60000, [&w, v, doc, require, all, value](bool shown) {
    require(shown, "the document is shown");
    if (!shown) return QCoreApplication::exit(2);
    QAction* faces = w.action("select.faces");
    auto chipShown = [&w](const QString& text) {
      for (auto* label : w.m_chips->findChildren<QLabel*>())
        if (label->text() == text && label->isVisibleTo(w.m_chips)) return true;
      return false;
    };
    auto segmentShown = [&w, faces] {
      for (auto* button : w.findChildren<SegmentButton*>())
        if (button->defaultAction() == faces) return !button->isHidden();
      return false;
    };
    auto threeD = [&w, faces, chipShown, segmentShown] {
      return faces->isVisible() && segmentShown() && chipShown(MainWindow::tr("Shaded + edges")) && chipShown(MainWindow::tr("Orthographic"));
    };
    auto twoD = [&w, v, faces, chipShown, segmentShown] {
      return v->drawingWords() && !faces->isVisible() && !segmentShown() && !chipShown(MainWindow::tr("Shaded + edges")) && !chipShown(MainWindow::tr("Orthographic")) &&
             v->selectionFilter() != Viewport::SelFilter::Face;
    };
    // The selection filters' names: a drawing's (groups, objects, points) or the 3D ones.
    auto filterWords = [&w](bool drawing) {
      const QString bodies = w.action("select.bodies")->text(), edges = w.action("select.edges")->text(), vertices = w.action("select.vertices")->text();
      if (!drawing) return bodies == i18n::t("Bodies") && edges == i18n::t("Edges") && vertices == i18n::t("Vertices");
      return bodies == QObject::tr("Groups") && edges == QObject::tr("Objects") && vertices == QObject::tr("Points") &&
             w.action("select.bodies")->toolTip().contains(QObject::tr("A layer's objects of one colour, picked together"));
    };
    constexpr int kPropKey = Qt::UserRole + 3;  // PropertiesPanel's kPropKeyRole: a row's property name
    auto propKeys = [&w] {
      QStringList keys;
      for (int i = 0; i < w.m_props->table()->topLevelItemCount(); ++i) keys << w.m_props->table()->topLevelItem(i)->data(0, kPropKey).toString();
      return keys;
    };
    auto propRow = [&w](const QString& label) -> std::optional<QString> {  // the value beside a label (a provided section's rows too)
      for (int i = 0; i < w.m_props->table()->topLevelItemCount(); ++i)
        if (w.m_props->table()->topLevelItem(i)->text(0) == label) return w.m_props->table()->topLevelItem(i)->text(1);
      return std::nullopt;
    };
    auto script = std::make_shared<Script>();
    if (!drawing2d::drawingOnly(doc->scene)) {  // a solid: 3D words until 2D mode
      QAction* flat = w.action("view.2d");
      script->add("a solid in 3D", [v, faces, require, threeD, filterWords] {
        faces->trigger();
        require(!v->drawingWords() && threeD() && filterWords(false), "a solid keeps the Faces filter, the display chips and the 3D words");
      });
      script->add("2D mode", [flat] { flat->setChecked(true); }, [twoD] { return twoD(); });
      script->add("2D mode off", [flat, require, filterWords] {
        require(filterWords(false), "2D mode takes the Faces filter (a Faces pick moves to Edges) and the display chips away; with no drawing the filters keep their names");
        flat->setChecked(false);
      }, [v, threeD] { return !v->drawingWords() && threeD(); });
      script->add("back", [require, filterWords] { require(filterWords(false), "leaving 2D mode brings them back"); });
    } else {
      auto at = std::make_shared<QPointF>();
      script->add("a drawing", [&w, v, doc, require, twoD, at, filterWords] {
        require(twoD(), "a drawing has no Faces filter or display chips, and the 2D words");
        require(filterWords(true), QString("the filters are named as a drawing's picks: %1, %2, %3")
                                       .arg(w.action("select.bodies")->text(), w.action("select.edges")->text(), w.action("select.vertices")->text()));
        const std::string lines = layerNamed(doc->scene, "Lines");
        Bnd_Box box;
        for (const auto& body : doc->scene.bodies_under(lines)) box.Add(opad::node_world_bbox(doc->doc, doc->scene, body));
        double x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);
        v->fitAll();
        *at = QPointF(v->widgetPoint({x0 + (x1 - x0) * 0.3, (y0 + y1) / 2, (z0 + z1) / 2}));
        w.action("select.edges")->trigger();
      }, [v] { return v->selectionFilter() == Viewport::SelFilter::Edge; });
      auto card = std::make_shared<QLabel*>(nullptr);
      script->add("hover a line", [&w, v, require, at, card] {
        const bool hovered = v->benchHover(*at);
        const QString text = w.m_statusHover->text(), expected = Viewport::tr("%1 on %2").arg(Viewport::tr("Line"), "Lines");
        require(hovered && text.startsWith(expected + " · ") && text.contains(units::format(units::Kind::Length, 100)),
                "a hovered line reads as a line on its layer with its length: " + text);
        *card = w.findChild<QLabel*>("rolloverCard");
      }, [card] { return *card && (*card)->isVisible(); });
      script->add("the rollover card", [&w, require, value, card] {
        const QString text = (*card)->text();
        require(text.contains(Viewport::tr("Line")) && text.contains("Lines") && text.contains(QObject::tr("Length")) && text.contains(QObject::tr("Lineweight")) &&
                    text.contains(QObject::tr("Drawing colour")),
                "after a rest the rollover card names the type, layer, colour, linetype, lineweight and length");
        (*card)->grab().save(value + ".card.png");
      });
      script->add("pick it", [&w, v, doc] {  // as a click on it selects it (a hidden window's posted clicks wait for a frame)
        opad::Ref line;
        line.body = doc->scene.bodies_under(layerNamed(doc->scene, "Lines")).at(0);
        line.kind = opad::Ref::Kind::Edge;
        line.index = 0;
        v->selectRefs({line});
        w.onViewportSelection();
      }, [v] { return !v->selection().empty(); });
      script->add("picked", [&w, require, value] {
        const QString text = w.m_statusSel->text();
        require(text == MainWindow::tr("%1 selected · %2").arg(1).arg(i18n::t("object")), "a picked line counts as an object: " + text);
        w.statusBar()->grab().save(value + ".status.png");
        w.action("inspect.properties")->trigger();
      }, [&w] { return w.m_propsPanel->isVisible(); });
      auto title = [&w] { return w.m_props->findChild<QLabel*>("panelTitle")->text(); };
      auto subtitle = [&w] {
        for (auto* label : w.m_props->findChildren<QLabel*>())
          if (label->objectName() == "secondary") return label->text();
        return QString();
      };
      script->add("its properties", [&w, require, value, title, subtitle, propKeys, propRow] {
        const QStringList keys = propKeys();
        require(title() == Viewport::tr("Line") && subtitle().contains("Lines") && subtitle().contains(i18n::t("object")) && keys.contains("length") &&
                    !keys.contains("vertices") && !keys.contains("adjacent_faces") && !keys.contains("axis"),
                QString("Properties name a picked line as a line on its layer, without solid words: %1 / %2 / %3").arg(title(), subtitle(), keys.join(",")));
        require(propRow(QObject::tr("Layer").toUpper()) && propRow(QObject::tr("Name")) == QChar(0x202A) + QString("Lines") + QChar(0x202C) &&
                    propRow(QObject::tr("Lineweight")).value_or(QString()).contains(QObject::tr("Default")),
                "Properties have the layer's section: " + propRow(QObject::tr("Name")).value_or("none"));
        w.m_propsPanel->grab().save(value + ".properties.png");
        w.action("select.bodies")->trigger();
      }, [v] { return v->selectionFilter() == Viewport::SelFilter::Body; });
      script->add("hover a body", [&w, v, require, at] {
        v->benchHover(QPointF(at->x() + 1, at->y()));
        v->benchHover(*at);
        const QString text = w.m_statusHover->text();
        require(text == Viewport::tr("Group on %1").arg("Lines"), "with the Bodies (Groups) filter a drawing body is a group on its layer: " + text);
      });
      script->add("pick the group", [&w, v, doc] {
        opad::Ref group;
        group.body = doc->scene.bodies_under(layerNamed(doc->scene, "Lines")).at(0);
        v->selectRefs({group});
        w.onViewportSelection();
        w.action("inspect.properties")->trigger();
      }, [&w, propKeys] { return w.m_propsPanel->isVisible() && propKeys().contains("objects"); });  // counted on a worker
      script->add("the group's properties", [&w, require, title, propKeys] {
        const QStringList keys = propKeys();
        require(w.m_statusSel->text() == MainWindow::tr("%1 selected · %2").arg(1).arg(i18n::t("group")) && title() == "Lines" && keys.contains("fills") &&
                    keys.contains("points") && !keys.contains("faces") && !keys.contains("edges") && !keys.contains("solid") && !keys.contains("volume"),
                QString("a picked group counts as one, its Properties count objects, fills and points: %1 / %2").arg(w.m_statusSel->text(), keys.join(",")));
      });
    }
    Script::run(&w, script, 0, require, [all] { QCoreApplication::exit(*all ? 0 : 2); });
  });
  return true;
}
