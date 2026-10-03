// Benches of the 2D measuring tools (drawing2d, UI-90): the Area tool. Cases in tools/bench_cases/drawing2d.py; the measure
// itself is tests/test_area.
#include <QAction>
#include <QElapsedTimer>
#include <QImage>
#include <QKeySequence>
#include <QLabel>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QStatusBar>
#include <QSettings>
#include <QToolButton>
#include <QTreeWidget>

#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <cmath>

#include "BenchRegistry.hpp"
#include "Drawing2D.hpp"
#include "Drawing2DBench.hpp"
#include "GuidedTool.hpp"
#include "MainWindow.hpp"
#include "PanelFooter.hpp"
#include "ToolPanel.hpp"
#include "Units.hpp"
#include "opad/geometry.hpp"

using namespace bench2d;

namespace {
// The Walls layer's edge from (x0, y0) to (x1, y1) (either way round), or its vertex at (x0, y0), in the drawing's own
// coordinates (the import may have moved it: the root's placement is undone).
opad::Ref drawingRef(const opad::Document& doc, const opad::Scene& scene, opad::Ref::Kind kind, double x0, double y0, double x1 = 0, double y1 = 0) {
  opad::Ref r;
  r.body = scene.bodies_under(layerNamed(scene, "Walls")).at(0);
  r.kind = kind;
  const gp_Trsf local = opad::trsf_from_mat(scene.world(r.body)).Inverted();
  TopTools_IndexedMapOfShape map;
  TopExp::MapShapes(opad::node_world_shape(doc, scene, r.body), kind == opad::Ref::Kind::Edge ? TopAbs_EDGE : TopAbs_VERTEX, map);
  auto same = [&](gp_Pnt p, double x, double y) { return std::hypot(p.Transformed(local).X() - x, p.Transformed(local).Y() - y) < 1e-6; };
  for (int i = 1; i <= map.Extent(); ++i) {
    if (kind == opad::Ref::Kind::Vertex) {
      if (same(BRep_Tool::Pnt(TopoDS::Vertex(map(i))), x0, y0)) return r.index = i - 1, r;
      continue;
    }
    const BRepAdaptor_Curve c(TopoDS::Edge(map(i)));
    const gp_Pnt a = c.Value(c.FirstParameter()), b = c.Value(c.LastParameter());
    if ((same(a, x0, y0) && same(b, x1, y1)) || (same(a, x1, y1) && same(b, x0, y0))) return r.index = i - 1, r;
  }
  return r;
}
}  // namespace

// OPAD_BENCH_AREA=<prefix> on a room of two cells (100 x 50 split at x = 60, the room.dxf of drawing2d.py). The Area tool
// (Inspect menu, Review ribbon) switches a drawing's Groups filter to Objects; one wall grows into the cell it bounds (3000
// mm², its outline and value drawn), Esc clears it; three walls of the other cell are open (two loose ends, nothing to pin),
// the fourth closes it (2000 mm²), which pins as an area measurement in an .opad (viewer mode: no pin); in a second room
// whose wall meets the others in their middles (T) the long wall is cut there and grows into the part at its middle (3000,
// not the smaller 2000); four lines drawn past each other's ends close once trimmed (2000); with the Points filter three
// corners make a triangle (2500 mm²); the command again ends the tool. <prefix>.prompt.png, .panel.png, .viewport.png.
OPAD_BENCH(OPAD_BENCH_AREA, area) {
  auto all = std::make_shared<bool>(true);
  Check require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: area: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
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
    require(shown && !layerNamed(doc->scene, "Walls").empty(), "the room is shown");
    if (!shown || layerNamed(doc->scene, "Walls").empty()) return QCoreApplication::exit(2);
    using Kind = opad::Ref::Kind;
    auto edge = [doc](double x0, double y0, double x1, double y1) { return drawingRef(doc->doc, doc->scene, Kind::Edge, x0, y0, x1, y1); };
    auto pick = [&w, v](const std::vector<opad::Ref>& refs) {  // as clicks select them (a hidden window's posted clicks wait for a frame)
      v->selectRefs(refs);
      w.onViewportSelection();
    };
    auto measured = [&w] { return !w.m_lastMeasure.is_null() && !w.m_measureJob; };
    auto rows = [&w] {
      QStringList out;
      if (auto* grid = w.m_toolSteps->findChild<QTreeWidget*>())
        for (int i = 0; i < grid->topLevelItemCount(); ++i) out << grid->topLevelItem(i)->text(0) + "=" + grid->topLevelItem(i)->text(1).remove(QChar(0x202A)).remove(QChar(0x202C));
      return out;
    };
    QAction* area = w.action("inspect.area");
    auto script = std::make_shared<Script>();
    script->add("start", [&w, v, area, require] {
      require(area && w.m_commands.inWorkspace("review").contains("inspect.area") && w.m_commands.find("inspect.area")->menuPath == "inspect",
              "Area is on the Review ribbon and in the Inspect menu");
      w.action("select.bodies")->trigger();
      area->trigger();
    }, [&w, v] { return w.m_tool.id == "area" && v->selectionFilter() == Viewport::SelFilter::Edge; });
    script->add("one wall", [&w, area, require, pick, edge] {
      require(area->isChecked() && w.m_prompt->isVisible() && w.m_toolPanel->isVisible(), "the tool runs: its command checked, the prompt and the panel shown, Groups became Objects");
      pick({edge(0, 0, 60, 0)});
    }, measured);
    script->add("grown", [&w, v, require, rows, value] {
      const opad::json& r = w.m_lastMeasure;
      const QString expected = units::format(units::Kind::Area, 3000);
      require(r.value("closed", false) && r.value("grown", false) && std::abs(r.value("value", 0.0) - 3000) < 1e-6 && std::abs(r.value("perimeter", 0.0) - 220) < 1e-6,
              QString("one wall grows into the cell it bounds: %1 mm², perimeter %2").arg(r.value("value", 0.0)).arg(r.value("perimeter", 0.0)));
      require(rows().value(0) == w.m_tool.title + "=" + expected && rows().join(",").contains(units::format(units::Kind::Length, 220)), "the panel shows the area and the perimeter: " + rows().join(", "));
      require(v->measurementCaptions().join(",").contains(expected), "the view labels the area inside it: " + v->measurementCaptions().join(", "));
      require(w.m_pinAction->isEnabled() == !w.m_doc->browse, "a closed area can be pinned in a document, not in viewer mode");
      require(PanelFooter::text(w.m_toolSteps->footer()->cancel()) == QObject::tr("Back"), "its footer's Esc button takes the last pick back");
    });
    script->add("shots", [&w, v, value] {  // a step later: the panel's new step rows are shown by a queued call
      w.m_prompt->grab().save(value + ".prompt.png");
      w.m_toolPanel->grab().save(value + ".panel.png");
      v->grabImage().save(value + ".viewport.png");
      w.toolEscape();
    }, [&w] { return w.m_toolPicks.empty() && w.m_lastMeasure.is_null() && w.m_tool.id == "area"; });
    script->add("three walls", [require, pick, edge] {
      require(true, "Esc clears the result and keeps the tool");
      pick({edge(60, 0, 100, 0), edge(100, 0, 100, 50), edge(100, 50, 60, 50)});
    }, measured);
    script->add("open", [&w, v, require, rows, pick, edge] {
      const opad::json& r = w.m_lastMeasure;
      require(!r.value("closed", true) && r.value("open_ends", 0) == 2 && !w.m_pinAction->isEnabled() && rows().join(",").contains(units::format(units::Kind::Length, 130)) &&
                  !v->measurementCaptions().join(",").contains(units::format(units::Kind::Area, 0)),
              "three walls are open: two loose ends, their length, no area and nothing to pin: " + rows().join(", "));
      w.toolEscape();
    }, [&w, measured] { return w.m_toolPicks.size() == 2 && measured(); });
    script->add("one back", [&w, require, pick, edge] {
      require(w.m_tool.id == "area" && !w.m_lastMeasure.value("closed", true), "Esc takes the last pick back, the tool stays");
      pick({edge(60, 0, 100, 0), edge(100, 0, 100, 50), edge(100, 50, 60, 50), edge(60, 50, 60, 0)});
    }, [&w, measured] { return measured() && w.m_lastMeasure.value("closed", false); });
    script->add("closed", [&w, require] {
      const opad::json& r = w.m_lastMeasure;
      require(std::abs(r.value("value", 0.0) - 2000) < 1e-6 && !r.value("grown", false), QString("the fourth wall closes it: %1 mm²").arg(r.value("value", 0.0)));
      if (!w.m_doc->browse) w.pinMeasurement();
    }, [&w] { return w.m_doc->browse || !w.m_doc->scene.measurements.empty(); });
    script->add("pinned", [&w, require, pick, edge] {
      if (!w.m_doc->browse) {
        const auto& m = w.m_doc->scene.measurements.back();
        require(m.kind == "area" && std::abs(m.result.value("value", 0.0) - 2000) < 1e-6 && m.refs.size() == 4, "pinned as an area measurement with its four walls");
      }
      pick({edge(200, 0, 300, 0)});
    }, measured);
    script->add("tee", [&w, require, pick, edge] {
      const opad::json& r = w.m_lastMeasure;
      require(r.value("closed", false) && r.value("grown", false) && std::abs(r.value("value", 0.0) - 3000) < 1e-6 && std::abs(r.value("perimeter", 0.0) - 220) < 1e-6,
              QString("a wall the dividing wall meets in its middle is cut there: the part at its middle grows into its room, %1 mm²").arg(r.value("value", 0.0)));
      pick({edge(395, 0, 455, 0), edge(450, -5, 450, 45), edge(455, 40, 395, 40), edge(400, 45, 400, -5)});
    }, measured);
    script->add("overshoot", [&w, require] {
      const opad::json& r = w.m_lastMeasure;
      require(r.value("closed", false) && r.value("trimmed", false) && std::abs(r.value("value", 0.0) - 2000) < 1e-6 && std::abs(r.value("perimeter", 0.0) - 180) < 1e-6,
              QString("four lines drawn past each other's ends close once trimmed: %1 mm², perimeter %2").arg(r.value("value", 0.0)).arg(r.value("perimeter", 0.0)));
      w.action("select.vertices")->trigger();
    }, [&w, v] { return v->selectionFilter() == Viewport::SelFilter::Vertex && w.m_toolPicks.empty(); });
    script->add("three corners", [doc, pick] {
      pick({drawingRef(doc->doc, doc->scene, Kind::Vertex, 0, 0), drawingRef(doc->doc, doc->scene, Kind::Vertex, 100, 0), drawingRef(doc->doc, doc->scene, Kind::Vertex, 100, 50)});
    }, measured);
    script->add("triangle", [&w, area, require] {
      const opad::json& r = w.m_lastMeasure;
      require(r.value("closed", false) && r.value("points", 0) == 3 && std::abs(r.value("value", 0.0) - 2500) < 1e-6, QString("three corners make a triangle: %1 mm²").arg(r.value("value", 0.0)));
      area->trigger();
    }, [&w, area] { return w.m_tool.id.isEmpty() && !area->isChecked() && !w.m_prompt->isVisible(); });
    Script::run(&w, script, 0, require, [all] { QCoreApplication::exit(*all ? 0 : 2); });
  });
  return true;
}

// OPAD_BENCH_OSNAP=<prefix> on the room with a circle (Holes) and a line across it (Axis), each layer its own body. In the
// Distance tool with the Points filter, F3 on (the default, its switch in the status bar): near a wall's middle the
// midpoint shows (its marker, "Midpoint" in the status) and a click picks it as a point; near where Axis crosses the
// dividing wall (two bodies) the intersection, picked as the second point (the distance between them is measured); the
// circle's centre, a quadrant and a point exactly on it; from a picked point, the foot of the perpendicular on the Axis
// line and the point where a line from a corner touches the circle (both measured); the sketch's kind switches apply
// (midpoint off: the nearest point instead); F3 off shows nothing, nor does the Objects filter. <prefix>.snap.png,
// .perpendicular.png.
OPAD_BENCH(OPAD_BENCH_OSNAP, osnap) {
  auto all = std::make_shared<bool>(true);
  Check require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: osnap: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
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
    require(shown && !layerNamed(doc->scene, "Axis").empty(), "the drawing is shown");
    if (!shown || layerNamed(doc->scene, "Axis").empty()) return QCoreApplication::exit(2);
    // World and widget points of drawing coordinates (the drawing was centred when opened).
    const opad::Mat4 placed = doc->scene.world(doc->scene.bodies_under(layerNamed(doc->scene, "Walls")).at(0));
    auto world = [placed](double x, double y) { return placed.apply({x, y, 0}); };
    auto widget = [v, world](double x, double y, double dx = 3, double dy = -2) { return QPointF(v->widgetPoint(world(x, y))) + QPointF(dx, dy); };
    auto same = [world](const opad::Vec3& p, double x, double y, double eps = 1e-6) {
      const opad::Vec3 q = world(x, y);
      return std::hypot(p[0] - q[0], p[1] - q[1]) + std::abs(p[2] - q[2]) < eps;
    };
    auto shown2 = [v](const QString& kind, opad::Vec3& at) {
      QString k;
      return v->shownSnap(at, &k) && k == kind;
    };
    auto click = [v](const QPointF& at) {
      for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent e(type, at, v->mapToGlobal(at), Qt::LeftButton, type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(v, &e);
      }
    };
    QAction* f3 = w.action("drawing2d.objectSnap");
    auto script = std::make_shared<Script>();
    script->add("start", [&w, v, f3, require] {
      auto* toggle = w.findChild<QToolButton*>("objectSnapToggle");
      require(f3 && f3->isChecked() && f3->shortcut() == QKeySequence("F3") && v->objectSnap() && toggle && toggle->defaultAction() == f3,
              "object snap is on by default, on F3, with its switch in the status bar");
      v->fitAll();
      w.action("inspect.distance")->trigger();
      w.action("select.vertices")->trigger();
    }, [&w, v] { return w.m_tool.id == "distance" && v->selectionFilter() == Viewport::SelFilter::Vertex && v->snapIndexesReady(); });
    auto at = std::make_shared<opad::Vec3>();
    script->add("midpoint", [&w, v, widget, same, shown2, at, require, value] {
      const bool hovered = v->benchSnap(widget(30, 0));
      require(hovered && shown2("midpoint", *at) && same(*at, 30, 0) && w.m_statusHover->text().startsWith(Viewport::snapWord("midpoint")),
              "near a wall's middle its midpoint shows, named in the status: " + w.m_statusHover->text());
      v->grabImage().save(value + ".snap.png");
    });
    script->add("pick it", [v, click, widget] { click(widget(30, 0)); }, [&w] { return w.m_toolPicks.size() == 1; });
    script->add("crossing", [&w, v, widget, same, shown2, at, click, require] {
      const opad::Ref& first = w.m_toolPicks.front();
      require(first.kind == opad::Ref::Kind::Point && same(first.point, 30, 0), "a click picks the midpoint as a point");
      QElapsedTimer clock;
      clock.start();
      const bool hovered = v->benchSnap(widget(60, 20));
      trace::log(QString("bench: osnap: a snap took %1 ms").arg(clock.elapsed()));
      require(hovered && shown2("intersection", *at) && same(*at, 60, 20), "where the Axis line crosses the dividing wall (another body) the intersection shows");
      click(widget(60, 20));
    }, [&w] { return w.m_toolPicks.size() == 2 && !w.m_lastMeasure.is_null() && !w.m_measureJob; });
    script->add("measured", [&w, v, widget, same, shown2, at, require] {
      require(std::abs(w.m_lastMeasure.value("value", 0.0) - std::hypot(30.0, 20.0)) < 1e-6, QString("the distance between the snapped points: %1").arg(w.m_lastMeasure.value("value", 0.0)));
      w.toolEscape();  // measure again
    }, [&w] { return w.m_toolPicks.empty(); });
    script->add("circle", [&w, v, widget, same, world, shown2, at, click, require] {
      require(v->benchSnap(widget(30, 25, 2, 2)) && shown2("center", *at) && same(*at, 30, 25), "the circle's centre");
      require(v->benchSnap(widget(30, 35, 3, 0)) && shown2("quadrant", *at) && same(*at, 30, 35), "its quadrant");
      const bool hovered = v->benchSnap(widget(30 + 10 * std::cos(M_PI / 3), 25 + 10 * std::sin(M_PI / 3), 2, 1)) && shown2("nearest", *at);
      const opad::Vec3 centre = world(30, 25);
      require(hovered && std::abs(std::hypot((*at)[0] - centre[0], (*at)[1] - centre[1]) - 10) < 1e-6, "a point exactly on the circle (nearest)");
      require(!v->benchSnap(widget(30, 20)) || !shown2("perpendicular", *at), "no perpendicular before a point is picked");
      v->benchSnap(widget(30, 25, 2, 2));  // the press picks the snap the cursor shows
      click(widget(30, 25, 2, 2));
    }, [&w] { return w.m_toolPicks.size() == 1; });
    script->add("perpendicular", [&w, v, widget, same, shown2, at, click, require, value] {
      require(!w.m_toolPicks.empty() && w.m_toolPicks.front().kind == opad::Ref::Kind::Point && same(w.m_toolPicks.front().point, 30, 25), "the centre picked as the first point");
      const bool hovered = v->benchSnap(widget(30, 20));
      require(hovered && shown2("perpendicular", *at) && same(*at, 30, 20) && w.m_statusHover->text().startsWith(Viewport::snapWord("perpendicular")),
              "from it, the foot of the perpendicular on the Axis line shows: " + w.m_statusHover->text());
      v->grabImage().save(value + ".perpendicular.png");
      click(widget(30, 20));
    }, [&w] { return w.m_toolPicks.size() == 2 && !w.m_lastMeasure.is_null() && !w.m_measureJob; });
    script->add("square distance", [&w, require] {
      require(std::abs(w.m_lastMeasure.value("value", 0.0) - 5) < 1e-6, QString("the distance square to the line: %1").arg(w.m_lastMeasure.value("value", 0.0)));
      w.toolEscape();
    }, [&w] { return w.m_toolPicks.empty(); });
    script->add("corner", [v, widget, click] {
      v->benchSnap(widget(0, 0, 2, 2));
      click(widget(0, 0, 2, 2));
    }, [&w] { return w.m_toolPicks.size() == 1; });
    const double turn = std::acos(10 / std::hypot(30.0, 25.0)), toward = std::atan2(-25.0, -30.0);
    const double tx = 30 + 10 * std::cos(toward + turn), ty = 25 + 10 * std::sin(toward + turn);  // where a line from (0, 0) touches the circle
    script->add("tangent", [&w, v, widget, same, shown2, at, click, require, tx, ty] {
      const bool hovered = v->benchSnap(widget(tx, ty, 2, 1));
      require(hovered && shown2("tangent", *at) && same(*at, tx, ty), QString("from the corner, where a line from it touches the circle shows (%1, %2)").arg(tx).arg(ty));
      click(widget(tx, ty, 2, 1));
    }, [&w] { return w.m_toolPicks.size() == 2 && !w.m_lastMeasure.is_null() && !w.m_measureJob; });
    script->add("touching distance", [&w, require] {
      require(std::abs(w.m_lastMeasure.value("value", 0.0) - std::sqrt(30.0 * 30 + 25 * 25 - 100)) < 1e-6,
              QString("the distance to the touching point: %1").arg(w.m_lastMeasure.value("value", 0.0)));
      w.toolEscape();
    }, [&w] { return w.m_toolPicks.empty(); });
    script->add("kinds", [v, widget, shown2, at, require] {
      QSettings().setValue("sketch/snap/midpoint", false);
      const bool hovered = v->benchSnap(widget(30, 0));
      require(hovered && shown2("nearest", *at), "with the sketch's Midpoint snap off the nearest point shows instead");
      QSettings().remove("sketch/snap/midpoint");
    });
    script->add("F3 off", [&w, v, f3, widget, require] {
      f3->trigger();
      require(!v->objectSnap() && !QSettings().value("view/objectSnap", true).toBool() && !v->benchSnap(widget(30, 0)) && !w.m_statusHover->text().startsWith(Viewport::snapWord("midpoint")),
              "F3 turns it off: nothing shows");
      f3->trigger();
      w.action("select.edges")->trigger();
    }, [v] { return v->selectionFilter() == Viewport::SelFilter::Edge; });
    script->add("objects", [&w, v, f3, widget, require] {
      require(v->objectSnap() && !v->benchSnap(widget(30, 0)), "F3 on again; with the Objects filter no point is snapped (the object is picked)");
      w.action("inspect.distance")->trigger();
    }, [&w] { return w.m_tool.id.isEmpty(); });
    script->add("radius", [&w] {
      w.action("select.vertices")->trigger();
      w.action("inspect.radius")->trigger();
    }, [&w, v] { return w.m_tool.id == "radius" && v->selectionFilter() == Viewport::SelFilter::Vertex; });
    script->add("no snap for a centre", [&w, v, widget, require] {
      require(v->snapPicks() == Viewport::SnapPicks::None && !v->benchSnap(widget(30, 25, 2, 2)) && !v->benchSnap(widget(30, 0)),
              "Radius in the Points filter shows no object snap: its click picks the circle's centre, not a free point");
      w.action("inspect.radius")->trigger();
    }, [&w, v] { return w.m_tool.id.isEmpty() && v->snapPicks() == Viewport::SnapPicks::None; });
    Script::run(&w, script, 0, require, [all] { QCoreApplication::exit(*all ? 0 : 2); });
  });
  return true;
}

// OPAD_BENCH_OSNAP_SKETCH=<prefix> on a document with one sketch on XY (a 100 x 50 rectangle of lines, a circle at (30, 25)
// radius 10, gui_benches' osnap-sketch): in the Distance tool with the Vertices filter the sketch's curves snap as a
// drawing's do (a side's midpoint, the circle's centre and quadrant), a click picks the centre, and from it the
// perpendicular foot on the top side; the distance between them (25). <prefix>.snap.png.
OPAD_BENCH(OPAD_BENCH_OSNAP_SKETCH, osnapSketch) {
  auto all = std::make_shared<bool>(true);
  Check require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: osnap-sketch: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    *all = *all && ok;
  };
  Viewport* v = w.m_viewport;
  AppDocument* doc = w.m_doc;
  auto widget = [v](double x, double y, double dx = 3, double dy = -2) { return QPointF(v->widgetPoint({x, y, 0})) + QPointF(dx, dy); };
  auto same = [](const opad::Vec3& p, double x, double y) { return std::hypot(p[0] - x, p[1] - y) + std::abs(p[2]) < 1e-6; };
  auto shown2 = [v](const QString& kind, opad::Vec3& at) {
    QString k;
    return v->shownSnap(at, &k) && k == kind;
  };
  auto click = [v](const QPointF& at) {
    for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
      QMouseEvent e(type, at, v->mapToGlobal(at), Qt::LeftButton, type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
      QCoreApplication::sendEvent(v, &e);
    }
  };
  require(doc->scene.sketches.size() == 1, "the document has its sketch");
  w.action("inspect.distance")->trigger();
  w.action("select.vertices")->trigger();
  v->fitAll();
  // Ready once the sketch is shown and indexed: its side's midpoint snaps.
  pollUntil(&w, [v, widget] { return v->benchSnap(widget(50, 0)); }, 60000, [&w, v, widget, same, shown2, click, require, all, value](bool ready) {
    auto at = std::make_shared<opad::Vec3>();
    require(ready && shown2("midpoint", *at) && same(*at, 50, 0), "a sketch side's midpoint snaps (the sketch indexed in its plane)");
    if (!ready) return QCoreApplication::exit(2);
    v->grabImage().save(value + ".snap.png");
    require(v->benchSnap(widget(30, 25, 2, 2)) && shown2("center", *at) && same(*at, 30, 25), "the circle's centre");
    require(v->benchSnap(widget(40, 25, 2, 1)) && shown2("quadrant", *at) && same(*at, 40, 25), "its quadrant");
    auto script = std::make_shared<Script>();
    script->add("pick", [v, widget, click] {
      v->benchSnap(widget(30, 25, 2, 2));  // the press picks the snap the cursor shows
      click(widget(30, 25, 2, 2));
    }, [&w] { return w.m_toolPicks.size() == 1; });
    script->add("perpendicular", [&w, v, widget, same, shown2, at, click, require] {
      require(!w.m_toolPicks.empty() && w.m_toolPicks.front().kind == opad::Ref::Kind::Point && same(w.m_toolPicks.front().point, 30, 25), "a click picks the centre as a point");
      require(v->benchSnap(widget(30, 50)) && shown2("perpendicular", *at) && same(*at, 30, 50), "from it, the foot of the perpendicular on the top side");
      click(widget(30, 50));
    }, [&w] { return w.m_toolPicks.size() == 2 && !w.m_lastMeasure.is_null() && !w.m_measureJob; });
    script->add("measured", [&w, require] {
      require(std::abs(w.m_lastMeasure.value("value", 0.0) - 25) < 1e-6, QString("the distance between them: %1").arg(w.m_lastMeasure.value("value", 0.0)));
      w.action("inspect.distance")->trigger();
    }, [&w] { return w.m_tool.id.isEmpty(); });
    Script::run(&w, script, 0, require, [all] { QCoreApplication::exit(*all ? 0 : 2); });
  });
  return true;
}

// OPAD_BENCH_READOUT=<prefix>. On a drawing far from (0, 0) (opened centred): the status bar's readout gives the cursor's
// X and Y as the file has them (1 000 060, 2 000 045 within a pixel), at a snapped point exactly that point (the Distance
// tool's Points filter, near a line's end), and nothing once the mouse leaves the view. On a model (the box): X, Y and Z
// of the surface under the cursor (the top face from above: Z 10), nothing over empty space. <prefix>.status.png.
OPAD_BENCH(OPAD_BENCH_READOUT, readout) {
  auto all = std::make_shared<bool>(true);
  Check require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: readout: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    *all = *all && ok;
  };
  Viewport* v = w.m_viewport;
  AppDocument* doc = w.m_doc;
  for (const auto& root : doc->scene.roots)  // the Engine .opad keeps its root hidden: shown here, in memory (never saved)
    if (const auto* n = doc->scene.node(root); n && !n->visible) doc->run("appearance", {{"target", root}, {"visible", true}});
  auto settled = [&w, v, doc] {
    int expected = 0;
    for (const auto& id : doc->scene.all_bodies()) expected += doc->scene.effectively_visible(id) && !doc->scene.node(id)->body_missing;
    return !w.m_displayJob && w.m_meshRemaining == 0 && v->displayedCount() + v->skippedCount() >= expected && expected > 0 && !v->looksPending();
  };
  pollUntil(&w, settled, 220000, [&w, v, doc, require, all, value](bool shown) {
    auto* readout = w.findChild<QLabel*>("cursorReadout");
    require(shown && readout && readout->text().isEmpty(), "the readout is in the status bar, empty while the mouse is elsewhere");
    if (!shown || !readout) return QCoreApplication::exit(2);
    auto numbers = [readout] {  // X, Y (and Z) as shown
      QList<double> out;
      static const QRegularExpression number("-?[0-9]+(\.[0-9]+)?");
      for (auto it = number.globalMatch(readout->text()); it.hasNext();) out << it.next().captured(0).toDouble();
      return out;
    };
    auto move = [v](const QPointF& at) {
      QMouseEvent e(QEvent::MouseMove, at, v->mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
      QCoreApplication::sendEvent(v, &e);
    };
    auto script = std::make_shared<Script>();
    const auto frames = drawing2d::drawingFrames(doc->doc, doc->scene);
    if (!frames.empty()) {
      const drawing2d::DrawingFrame frame = frames.front();
      auto widget = [v, frame](double x, double y, double dx = 0, double dy = 0) { return QPointF(v->widgetPoint(drawing2d::fromDrawing(frame, {x, y, 0}))) + QPointF(dx, dy); };
      script->add("over the drawing", [v, move, widget] {
        v->fitAll();
        move(widget(1000060, 2000045));
      }, [readout] { return !readout->text().isEmpty(); });
      script->add("drawing coordinates", [&w, v, readout, numbers, require, value, move, widget] {
        const QList<double> n = numbers();
        const double tolerance = 2 * v->pixelSize();
        require(n.size() >= 2 && std::abs(n[0] - 1000060) < tolerance && std::abs(n[1] - 2000045) < tolerance && !readout->text().contains("Z"),
                QString("X and Y as the file has them, far from (0, 0): %1 (within %2)").arg(readout->text()).arg(tolerance));
        w.statusBar()->grab().save(value + ".status.png");
        w.action("inspect.distance")->trigger();
        w.action("select.vertices")->trigger();
      }, [&w, v] { return w.m_tool.id == "distance" && v->selectionFilter() == Viewport::SelFilter::Vertex && v->snapIndexesReady(); });
      script->add("snapped", [v, move, widget] {
        v->benchSnap(widget(1000010, 2000020, 3, -2));
        move(widget(1000010, 2000020, 3, -2));
      }, [readout] { return readout->toolTip().contains(QObject::tr("At the snapped point")); });
      script->add("exactly", [&w, readout, numbers, require] {
        const QList<double> n = numbers();
        require(n.size() >= 2 && std::abs(n[0] - 1000010) < 1e-3 && std::abs(n[1] - 2000020) < 1e-3, "at a snapped point the readout is that point: " + readout->text());
        w.action("inspect.distance")->trigger();
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(w.m_viewport, &leave);
      }, [readout] { return readout->text().isEmpty(); });
    } else {
      script->add("over the model", [v, move] {
        v->standardView("top");
        v->fitAll();
        move(QPointF(v->width() / 2.0, v->height() / 2.0));
      }, [readout] { return !readout->text().isEmpty(); });
      script->add("model coordinates", [v, doc, readout, numbers, require, move, value, &w] {
        const QList<double> n = numbers();
        const bool box = doc->scene.all_bodies().size() == 1;  // the box fixture: its top face from above; a big model: any surface
        require(n.size() >= 3 && (!box || std::abs(n[2] - 10) < 1e-6) && readout->text().contains("Z"), "on a model: X, Y and Z of the surface under the cursor: " + readout->text());
        // What a readout costs while the mouse moves (one ray into the view's picking structures): on the Engine too.
        qint64 worst = 0;
        opad::Vec3 at;
        for (int i = 0; i < 60; ++i) {
          QElapsedTimer clock;
          clock.start();
          v->pointUnder(QPointF(v->width() * (0.2 + 0.6 * (i % 10) / 9.0), v->height() * (0.2 + 0.6 * (i / 10) / 5.0)), at);
          worst = std::max(worst, clock.elapsed());
        }
        require(worst < 50, QString("60 readouts across the view on %1 bodies, the slowest %2 ms").arg(doc->scene.all_bodies().size()).arg(worst));
        w.statusBar()->grab().save(value + ".status.png");
        move(QPointF(3, v->height() - 3));
      }, [readout] { return readout->text().isEmpty(); });
      script->add("empty space", [require] { require(true, "over empty space the readout is empty"); });
    }
    Script::run(&w, script, 0, require, [all] { QCoreApplication::exit(*all ? 0 : 2); });
  });
  return true;
}
