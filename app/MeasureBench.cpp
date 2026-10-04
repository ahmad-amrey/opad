// The guided measuring tools' bench (viewer area, T2); case in tools/bench_cases/viewer.py.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QTimer>
#include <QTreeWidget>

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_NurbsConvert.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepTools.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>

#include <functional>
#include <sstream>

#include "BenchRegistry.hpp"
#include "MainWindow.hpp"
#include "Units.hpp"
#include "opad/geometry.hpp"

namespace {
// Turns the event loop until `done` holds or `ms` have passed.
bool until(const std::function<bool()>& done, int ms) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < ms) {
    QEventLoop loop;
    QTimer::singleShot(20, &loop, &QEventLoop::quit);
    loop.exec();
  }
  return done();
}

std::string brepText(const TopoDS_Shape& shape) {
  std::ostringstream out;
  BRepTools::Write(shape, out);
  return out.str();
}
}  // namespace

// OPAD_BENCH_MEASURE=<prefix> on an empty document. UI-50: a rod written as B-splines (r 6.5 mm) and a B-spline box are
// imported; the Radius tool on the rod's side reads R 6.500 mm and Ø 13.000 mm and says the surface was recognised as a
// cylinder; on a box face the panel says why it cannot be measured, the pick is taken back, and the next pick clears it.
// <prefix>.radius.png / .radius-error.png: the tool panel.
OPAD_BENCH(OPAD_BENCH_MEASURE, measure) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: measure: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  };
  auto row = [&w](const QString& key) {
    const auto* grid = w.m_toolSteps->findChild<QTreeWidget*>();
    for (int i = 0; grid && i < grid->topLevelItemCount(); ++i)
      if (grid->topLevelItem(i)->text(0) == key) return grid->topLevelItem(i)->toolTip(1);
    return QString();
  };
  auto settle = [](int ms = 400) { until([] { return false; }, ms); };
  const QString prefix = value;
  units::setPrecision(3, false, 0);
  const TopoDS_Shape rod = BRepBuilderAPI_NurbsConvert(BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 6.5, 40).Shape(), Standard_True).Shape();
  const TopoDS_Shape box = BRepBuilderAPI_NurbsConvert(BRepPrimAPI_MakeBox(gp_Pnt(30, -10, 0), 20, 20, 20).Shape(), Standard_True).Shape();
  try {
    w.m_doc->run("import_brep", {{"brep", brepText(rod)}, {"name", "Rod"}});
    w.m_doc->run("import_brep", {{"brep", brepText(box)}, {"name", "Block"}});
  } catch (const std::exception& e) {
    require(false, QString("importing the B-spline bodies: %1").arg(e.what()));
    QCoreApplication::exit(2);
    return true;
  }
  std::string rodId, boxId;
  for (const auto& id : w.m_doc->scene.all_bodies()) (w.m_doc->nodeName(id) == "Rod" ? rodId : boxId) = id;
  const bool shown = until([&w] { return w.m_viewport->displayedCount() >= 2 && w.m_viewport->remainingBodies() == 0; }, 30000);
  require(shown && !rodId.empty() && !boxId.empty(), "the B-spline rod and block are displayed");
  if (!shown || rodId.empty() || boxId.empty()) return QCoreApplication::exit(2), true;
  w.m_viewport->fitAll();
  // The rod's side: the B-spline face half way up.
  int side = -1;
  {
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(rod, TopAbs_FACE, faces);
    for (int i = 1; i <= faces.Extent() && side < 0; ++i) {
      BRepAdaptor_Surface s(TopoDS::Face(faces(i)));
      const gp_Pnt mid = s.Value((s.FirstUParameter() + s.LastUParameter()) / 2, (s.FirstVParameter() + s.LastVParameter()) / 2);
      if (mid.Z() > 1 && mid.Z() < 39) side = i - 1;
    }
  }
  auto pick = [&w](const std::string& body, opad::Ref::Kind kind, int index) {
    opad::Ref r;
    r.body = body;
    r.kind = kind;
    r.index = index;
    w.m_viewport->selectRefs({r});
    w.onViewportSelection();  // what a click reports
  };

  // ---- UI-50: radius of a B-spline cylinder; an error in the panel
  w.startTool("radius");
  until([&w] { return w.m_viewport->selectionFilter() == Viewport::SelFilter::Face; }, 5000);
  settle();
  pick(rodId, opad::Ref::Kind::Face, side);
  until([&w] { return !w.m_lastMeasure.is_null() || !w.m_toolError.isEmpty(); }, 20000);
  require(row(w.m_tool.title) == "6.500 mm" && row("Diameter") == "13.000 mm", "the B-spline rod's radius reads " + row(w.m_tool.title) + ", diameter " + row("Diameter"));
  require(row("Recognised as").startsWith("cylinder, within "), "the panel says the side was recognised: " + row("Recognised as"));
  const QStringList captions = w.m_viewport->measurementCaptions();
  require(captions.contains("R 6.500 mm"), "the view's label reads " + captions.join(" | "));
  settle(200);
  w.m_toolPanel->grab().save(prefix + ".radius.png");
  w.toolEscape();  // the result goes: measure again
  until([&w] { return w.m_lastMeasure.is_null() && w.m_toolPicks.empty(); }, 5000);
  pick(boxId, opad::Ref::Kind::Face, 0);
  until([&w] { return !w.m_toolError.isEmpty(); }, 20000);
  const auto* error = w.m_toolSteps->findChild<QLabel*>("toolError");
  require(error && error->isVisibleTo(w.m_toolSteps) && error->text().contains("has no radius") && error->text().contains("Block"),
          "a face with no radius is explained in the panel: " + (error ? error->text() : QString()));
  require(w.m_toolPicks.empty() && w.m_lastMeasure.is_null() && w.m_viewport->selection().empty(), "and the pick is asked for again");
  settle(200);
  w.m_toolPanel->grab().save(prefix + ".radius-error.png");
  pick(rodId, opad::Ref::Kind::Face, side);
  require(w.m_toolError.isEmpty() && error && !error->isVisibleTo(w.m_toolSteps), "the next pick clears the error");
  until([&w] { return !w.m_lastMeasure.is_null(); }, 20000);
  require(row(w.m_tool.title) == "6.500 mm", "and is measured: " + row(w.m_tool.title));
  w.cancelTool();

  QCoreApplication::exit(all ? 0 : 2);
  return true;
}
