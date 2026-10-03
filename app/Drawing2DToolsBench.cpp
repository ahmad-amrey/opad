// Benches of the 2D measuring tools (drawing2d, UI-90): the Area tool. Cases in tools/bench_cases/drawing2d.py; the measure
// itself is tests/test_area.
#include <QAction>
#include <QImage>
#include <QKeySequence>
#include <QLabel>
#include <QMouseEvent>
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
#include "Drawing2DBench.hpp"
#include "GuidedTool.hpp"
#include "MainWindow.hpp"
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
// the fourth closes it (2000 mm²), which pins as an area measurement in an .opad (viewer mode: no pin); with the Points
// filter three corners make a triangle (2500 mm²); the command again ends the tool. <prefix>.prompt.png, .panel.png,
// .viewport.png.
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
      pick({edge(60, 0, 100, 0), edge(100, 0, 100, 50), edge(100, 50, 60, 50), edge(60, 50, 60, 0)});
    }, [&w, measured] { return measured() && w.m_lastMeasure.value("closed", false); });
    script->add("closed", [&w, require] {
      const opad::json& r = w.m_lastMeasure;
      require(std::abs(r.value("value", 0.0) - 2000) < 1e-6 && !r.value("grown", false), QString("the fourth wall closes it: %1 mm²").arg(r.value("value", 0.0)));
      if (!w.m_doc->browse) w.pinMeasurement();
    }, [&w] { return w.m_doc->browse || !w.m_doc->scene.measurements.empty(); });
    script->add("pinned", [&w, require] {
      if (!w.m_doc->browse) {
        const auto& m = w.m_doc->scene.measurements.back();
        require(m.kind == "area" && std::abs(m.result.value("value", 0.0) - 2000) < 1e-6 && m.refs.size() == 4, "pinned as an area measurement with its four walls");
      }
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
// circle's centre, a quadrant and a point exactly on it; the sketch's kind switches apply (midpoint off: the nearest point
// instead); F3 off shows nothing, nor does the Objects filter. <prefix>.snap.png.
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
    script->add("circle", [v, widget, same, world, shown2, at, require] {
      require(v->benchSnap(widget(30, 25, 2, 2)) && shown2("center", *at) && same(*at, 30, 25), "the circle's centre");
      require(v->benchSnap(widget(30, 35, 3, 0)) && shown2("quadrant", *at) && same(*at, 30, 35), "its quadrant");
      const bool hovered = v->benchSnap(widget(30 + 10 * std::cos(M_PI / 3), 25 + 10 * std::sin(M_PI / 3), 2, 1)) && shown2("nearest", *at);
      const opad::Vec3 centre = world(30, 25);
      require(hovered && std::abs(std::hypot((*at)[0] - centre[0], (*at)[1] - centre[1]) - 10) < 1e-6, "a point exactly on the circle (nearest)");
    });
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
    Script::run(&w, script, 0, require, [all] { QCoreApplication::exit(*all ? 0 : 2); });
  });
  return true;
}
