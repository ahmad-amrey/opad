// Benches of the 2D measuring tools (drawing2d, UI-90): the Area tool. Cases in tools/bench_cases/drawing2d.py; the measure
// itself is tests/test_area.
#include <QAction>
#include <QImage>
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
