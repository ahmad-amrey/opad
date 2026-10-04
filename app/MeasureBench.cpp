// The guided measuring tools' bench (viewer area, T2); case in tools/bench_cases/viewer.py.
#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QToolButton>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QTimer>
#include <QTreeWidget>

#include <BRepAdaptor_Curve.hxx>
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
#include "I18n.hpp"
#include "MainWindow.hpp"
#include "Theme.hpp"
#include "Notes.hpp"
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
// <prefix>.radius.png / .radius-error.png: the tool panel. UI-144 on a plain plate and pin: a click's XYZ is listed while the
// next pick is awaited; Distance between the plate's side and the pin in its three modes from the panel's buttons
// (minimum 7, centre to centre 10 with the centres named, maximum 19.846 within its accuracy) with the measured points' XYZ
// and the view's caption; Length and area on the plate's top (area, perimeter) and on the pin's rim (length, the loops of
// the cap and the side); the earlier results listed with Copy and Pin (a measurement op of that result). Points and Δ in
// the axes of a turned component the pick lies in, its arrows in the view along them. <prefix>.modes.png / .length.png /
// .history.png / .frame.png.
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
  // The reason is the core's fixed text and what was picked, each with an Arabic entry (the whole sentence had none).
  const auto arabic = i18n::table("ar", {":/i18n"});
  require(error && error->text().endsWith("(need a cylindrical/spherical face or circular edge); it is a bspline face") &&
              arabic.contains("reference has no radius (need a cylindrical/spherical face or circular edge)") && arabic.contains("%1; it is a %2 %3") &&
              arabic.contains("bspline") && arabic.contains("face") && arabic.contains("%1 cannot be measured: %2"),
          "the reason says what was picked, and each part of it has Arabic: " + (error ? error->text() : QString()));
  settle(200);
  w.m_toolPanel->grab().save(prefix + ".radius-error.png");
  pick(rodId, opad::Ref::Kind::Face, side);
  require(w.m_toolError.isEmpty() && error && !error->isVisibleTo(w.m_toolSteps), "the next pick clears the error");
  until([&w] { return !w.m_lastMeasure.is_null(); }, 20000);
  require(row(w.m_tool.title) == "6.500 mm", "and is measured: " + row(w.m_tool.title));
  w.cancelTool();

  // ---- UI-144: picked-point XYZ, Distance modes, Length and area, history with Copy and Pin
  const TopoDS_Shape plate = BRepPrimAPI_MakeBox(gp_Pnt(60, 0, 0), 20, 20, 10).Shape();
  const TopoDS_Shape pin = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(90, 10, 0), gp_Dir(0, 0, 1)), 3, 10).Shape();
  w.m_doc->run("import_brep", {{"brep", brepText(plate)}, {"name", "Plate"}});
  w.m_doc->run("import_brep", {{"brep", brepText(pin)}, {"name", "Pin"}});
  std::string plateId, pinId;
  for (const auto& id : w.m_doc->scene.all_bodies()) {
    if (w.m_doc->nodeName(id) == "Plate") plateId = id;
    if (w.m_doc->nodeName(id) == "Pin") pinId = id;
  }
  require(until([&w] { return w.m_viewport->displayedCount() >= 4 && w.m_viewport->remainingBodies() == 0; }, 30000) && !plateId.empty() && !pinId.empty(),
          "the plate and the pin are displayed");
  auto faceWhere = [](const TopoDS_Shape& shape, const std::function<bool(const BRepAdaptor_Surface&)>& test) {
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    for (int i = 1; i <= faces.Extent(); ++i)
      if (test(BRepAdaptor_Surface(TopoDS::Face(faces(i))))) return i - 1;
    return -1;
  };
  const int plateSide = faceWhere(plate, [](const BRepAdaptor_Surface& s) { return s.GetType() == GeomAbs_Plane && std::abs(s.Plane().Axis().Direction().X()) > 0.5 && s.Value(s.FirstUParameter(), s.FirstVParameter()).X() > 79; });
  const int plateTop = faceWhere(plate, [](const BRepAdaptor_Surface& s) { return s.GetType() == GeomAbs_Plane && std::abs(s.Plane().Axis().Direction().Z()) > 0.5 && s.Value(s.FirstUParameter(), s.FirstVParameter()).Z() > 9; });
  const int pinSide = faceWhere(pin, [](const BRepAdaptor_Surface& s) { return s.GetType() == GeomAbs_Cylinder; });
  int pinRim = -1;
  {
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(pin, TopAbs_EDGE, edges);
    for (int i = 1; i <= edges.Extent() && pinRim < 0; ++i)
      if (BRepAdaptor_Curve(TopoDS::Edge(edges(i))).GetType() == GeomAbs_Circle) pinRim = i - 1;
  }
  w.m_viewport->fitAll();
  w.startTool("distance");
  bool filtered = false;
  auto once = QObject::connect(w.m_viewport, &Viewport::filterApplied, &w, [&filtered] { filtered = true; });
  if (w.m_viewport->selectionFilter() != Viewport::SelFilter::Face) w.action("select.faces")->trigger();
  else filtered = true;
  until([&filtered] { return filtered; }, 10000);
  QObject::disconnect(once);
  settle();
  // A real click on the plate: its point is listed while the second pick is awaited.
  int px = 0, py = 0;
  const bool onPlate = w.m_viewport->benchBodyPoint(plateId, px, py);
  if (onPlate) w.m_viewport->benchClickAt(QPointF(px, py) / w.m_viewport->displayScale());
  until([&w] { return w.m_toolPicks.size() == 1; }, 5000);
  require(onPlate && w.m_toolPicks.size() == 1 && row("Pick 1 at").startsWith("(") && row("Pick 1 at").endsWith(" mm"), "a click's point is listed: " + row("Pick 1 at"));
  w.toolEscape();  // one step back: the tool waits for its first pick again
  until([&w] { return w.m_toolPicks.empty(); }, 5000);
  auto pickTwo = [&](const opad::Ref& a, const opad::Ref& b) {
    w.m_viewport->selectRefs({a, b});
    w.onViewportSelection();
  };
  auto faceRef = [](const std::string& body, int index) {
    opad::Ref r;
    r.body = body;
    r.kind = opad::Ref::Kind::Face;
    r.index = index;
    return r;
  };
  auto modeButton = [&w](int i) {
    const auto* row = w.m_toolSteps->findChild<QWidget*>("toolModes");
    const auto buttons = row ? row->findChildren<QAbstractButton*>() : QList<QAbstractButton*>();
    return i < buttons.size() ? buttons[i] : nullptr;
  };
  if (auto* minimum = modeButton(0)) minimum->click();
  w.m_lastMeasure = opad::json();
  pickTwo(faceRef(plateId, plateSide), faceRef(pinId, pinSide));
  until([&w] { return !w.m_lastMeasure.is_null(); }, 20000);
  require(row("Distance") == "7.000 mm" && modeButton(2) && modeButton(0)->isChecked(), "minimum between the plate's side and the pin: " + row("Distance"));
  if (!modeButton(2)) return QCoreApplication::exit(2), true;
  modeButton(1)->click();
  until([&w] { return w.m_lastMeasure.is_object() && w.m_lastMeasure.value("mode", "") == "center"; }, 20000);
  require(row("Centre to centre") == "10.000 mm" && w.m_settings.value("measure/distanceMode").toInt() == 1, "centre to centre from the panel's button: " + row("Centre to centre"));
  require(row("Point 1 (face centroid)") == "(80.000, 10.000, 5.000) mm" && row("Point 2 (axis)") == "(90.000, 10.000, 5.000) mm",
          "the centres it took, world XYZ: " + row("Point 1 (face centroid)") + " / " + row("Point 2 (axis)"));
  require(w.m_viewport->measurementCaptions().contains("Centre to centre 10.000 mm"), "the view's caption: " + w.m_viewport->measurementCaptions().join(" | "));
  modeButton(2)->click();
  until([&w] { return w.m_lastMeasure.is_object() && w.m_lastMeasure.value("mode", "") == "max"; }, 20000);
  const double farthest = w.m_lastMeasure.value("value", 0.0), expected = std::hypot(std::hypot(10.0, 10.0) + 3, 10.0);
  require(std::abs(farthest - expected) <= w.m_lastMeasure.value("tolerance_mm", 0.0) + 1e-3 && row("Accuracy").startsWith("within "),
          QString("maximum %1 (expected %2) and its accuracy: %3").arg(farthest, 0, 'f', 4).arg(expected, 0, 'f', 4).arg(row("Accuracy")));
  settle(200);
  w.m_toolPanel->grab().save(prefix + ".modes.png");
  modeButton(0)->click();  // the setting goes back to the minimum for the next run
  until([&w] { return !w.m_lastMeasure.is_null() && !w.m_lastMeasure.contains("mode"); }, 20000);
  w.cancelTool();
  w.m_viewport->benchClickAt(QPointF(4, w.m_viewport->height() - 4));  // on nothing: no click point is left over for the picks below
  // Length and area: the plate's top, then the pin's rim.
  w.startTool("length");
  until([&w] { return w.m_tool.id == "length"; }, 2000);
  w.m_viewport->selectRefs({faceRef(plateId, plateTop)});
  w.onViewportSelection();
  until([&w] { return !w.m_lastMeasure.is_null(); }, 20000);
  require(row("Area") == QString::fromUtf8("400.000 mm²") && row("Perimeter") == "80.000 mm" && row("Centroid") == "(70.000, 10.000, 10.000) mm",
          "the plate's top: area " + row("Area") + ", perimeter " + row("Perimeter"));
  require(w.m_viewport->measurementCaptions().contains(QString::fromUtf8("A 400.000 mm²")), "the view's caption: " + w.m_viewport->measurementCaptions().join(" | "));
  filtered = false;
  once = QObject::connect(w.m_viewport, &Viewport::filterApplied, &w, [&filtered] { filtered = true; });
  w.action("select.edges")->trigger();
  until([&filtered] { return filtered; }, 10000);
  QObject::disconnect(once);
  opad::Ref rim;
  rim.body = pinId;
  rim.kind = opad::Ref::Kind::Edge;
  rim.index = pinRim;
  w.m_viewport->selectRefs({rim});
  w.onViewportSelection();
  until([&w] { return w.m_lastMeasure.is_object() && w.m_lastMeasure.value("kind", "") == "length"; }, 20000);
  const QString loops = row("Loop on face 0") + " " + row("Loop on face 1") + " " + row("Loop on face 2");
  require(row("Length") == "18.850 mm" && loops.contains("18.850 mm") && loops.contains("37.699 mm"), "the pin's rim: length " + row("Length") + ", loops " + loops.simplified());
  settle(200);
  w.m_toolPanel->grab().save(prefix + ".length.png");
  // History: the earlier results, newest first, below the current one.
  const auto* history = w.m_toolSteps->findChild<QTreeWidget*>("toolHistory");
  QStringList titles;
  for (int i = 0; history && i < history->topLevelItemCount(); ++i) titles << history->topLevelItem(i)->text(0);
  require(history && history->isVisibleTo(w.m_toolSteps) && titles.size() >= 5 && titles.value(0) == "Area" && titles.contains("Centre to centre") && titles.contains("Maximum distance"),
          "the earlier results are listed: " + titles.join(", "));
  const int centre = titles.indexOf("Centre to centre");
  auto rowButton = [history](int i, const char* name) {
    QWidget* actions = history ? history->itemWidget(history->topLevelItem(i), 2) : nullptr;
    return actions ? actions->findChild<QToolButton*>(name) : nullptr;
  };
  QApplication::clipboard()->clear();
  if (auto* copy = rowButton(centre, "historyCopy")) copy->click();
  require(QApplication::clipboard()->text().startsWith("Centre to centre\t10.000 mm"), "Copy on a row copies that result: " + QApplication::clipboard()->text().section('\n', 0, 0));
  const size_t ops = w.m_doc->doc.ops.size();
  if (auto* pinIt = rowButton(centre, "historyPin")) pinIt->click();
  const auto& last = w.m_doc->doc.ops.back();
  require(w.m_doc->doc.ops.size() == ops + 1 && last.type == "measurement" && last.data.value("result", opad::json()).value("mode", "") == "center",
          "Pin on a row pins that result as a measurement op");
  require(rowButton(centre, "historyPin") && !rowButton(centre, "historyPin")->isEnabled(), "and the row says it is pinned");
  // Another theme: the earlier results are made again in its colours (they kept the old theme's titles and icons).
  const bool dark = theme::current().dark;
  theme::apply(!dark);
  const QColor title = history && history->topLevelItemCount() ? history->topLevelItem(0)->foreground(0).color() : QColor();
  const bool themed = title == theme::current().fg2 && history->topLevelItemCount() == titles.size() && rowButton(centre, "historyPin") && !rowButton(centre, "historyPin")->isEnabled();
  theme::apply(dark);
  require(themed, "a theme change makes the earlier results again in its colours, the pinned row still pinned");
  settle(200);
  w.m_toolPanel->grab().save(prefix + ".history.png");
  w.cancelTool();

  // ---- UI-144: points in the axes of the first pick's component. A 10 mm cube in the component Lid, turned 90 degrees
  // about Z and moved 100 mm along X: its top's centroid is (95, 5, 10) in the world and (5, 5, 10) in the Lid's axes; the
  // centre to centre distance from its -X side to its top is Δ (0, +5, +5) in the world and (+5, 0, +5) in the Lid's.
  const opad::json made = w.m_doc->run("component", {{"name", "Lid"}});
  const std::string lid = made.value("id", "");
  w.m_doc->run("transform", {{"target", lid}, {"matrix", {0, -1, 0, 100, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}});
  const TopoDS_Shape cube = BRepPrimAPI_MakeBox(10, 10, 10).Shape();
  w.m_doc->run("import_brep", {{"brep", brepText(cube)}, {"name", "Cap"}, {"parent", lid}});
  std::string capId;
  for (const auto& id : w.m_doc->scene.all_bodies())
    if (w.m_doc->nodeName(id) == "Cap") capId = id;
  require(until([&w] { return w.m_viewport->displayedCount() >= 5 && w.m_viewport->remainingBodies() == 0; }, 30000) && !capId.empty(), "the cap in the turned component is displayed");
  const int capTop = faceWhere(cube, [](const BRepAdaptor_Surface& s) { return std::abs(s.Plane().Axis().Direction().Z()) > 0.5 && s.Value(s.FirstUParameter(), s.FirstVParameter()).Z() > 9; });
  const int capSide = faceWhere(cube, [](const BRepAdaptor_Surface& s) { return std::abs(s.Plane().Axis().Direction().X()) > 0.5 && s.Value(s.FirstUParameter(), s.FirstVParameter()).X() < 1; });
  auto* frames = w.m_toolSteps->findChild<QComboBox*>("toolFrame");
  auto frameShown = [&w, frames] { return frames && frames->parentWidget()->isVisibleTo(w.m_toolSteps); };
  if (frames) frames->setCurrentIndex(0);
  w.m_measureFrame = 0;
  w.startTool("length");
  filtered = false;
  once = QObject::connect(w.m_viewport, &Viewport::filterApplied, &w, [&filtered] { filtered = true; });
  w.action("select.faces")->trigger();
  until([&filtered] { return filtered; }, 10000);
  QObject::disconnect(once);
  w.m_viewport->selectRefs({faceRef(plateId, plateTop)});
  w.onViewportSelection();
  until([&w] { return !w.m_lastMeasure.is_null(); }, 20000);
  require(!frameShown() && row("Coordinates in").isEmpty(), "a pick at the root offers no component axes");
  w.toolEscape();
  until([&w] { return w.m_lastMeasure.is_null() && w.m_toolPicks.empty(); }, 5000);
  w.m_viewport->selectRefs({faceRef(capId, capTop)});
  w.onViewportSelection();
  until([&w] { return !w.m_lastMeasure.is_null(); }, 20000);
  require(frameShown() && frames->count() == 2 && frames->itemText(1) == "Lid (component)" && row("Centroid") == "(95.000, 5.000, 10.000) mm" && row("Coordinates in").isEmpty(),
          "the cap's top in world axes: " + row("Centroid") + ", the choice offers " + (frames ? frames->itemText(1) : QString()));
  if (frames) frames->setCurrentIndex(1);
  require(row("Centroid") == "(5.000, 5.000, 10.000) mm" && row("Coordinates in") == "Lid" && w.m_settings.value("measure/frame").toInt() == 1,
          "in the Lid's axes: " + row("Centroid") + " (" + row("Coordinates in") + ")");
  settle(200);
  w.m_toolPanel->grab().save(prefix + ".frame.png");
  w.pinMeasurement();  // its card in Annotations reads as an area
  QStringList cards;
  for (const auto* card : w.m_annotations->findChildren<NoteCard*>())
    if (card->note().measurement) cards << card->note().value;
  require(cards.contains(QString::fromUtf8("area measurement - 100.000 mm²")), "a pinned area's card reads in mm²: " + cards.join(" | "));
  w.cancelTool();
  w.startTool("distance");
  if (auto* centres = modeButton(1)) centres->click();
  pickTwo(faceRef(capId, capSide), faceRef(capId, capTop));
  until([&w] { return w.m_lastMeasure.is_object() && w.m_lastMeasure.value("mode", "") == "center"; }, 20000);
  QStringList shownCaptions = w.m_viewport->measurementCaptions();
  require(row("ΔX") == "+5.000 mm" && row("ΔY") == "0.000 mm" && row("ΔZ") == "+5.000 mm" && row("Point 1 (face centroid)") == "(0.000, 5.000, 5.000) mm"
          && shownCaptions.contains("ΔX +5.000 mm") && shownCaptions.contains("ΔZ +5.000 mm") && !shownCaptions.filter("ΔY").size(),
          QString("centre to centre in the Lid's axes: Δ %1 %2 %3, the view's arrows %4").arg(row("ΔX"), row("ΔY"), row("ΔZ"), shownCaptions.join(" | ")));
  if (frames) frames->setCurrentIndex(0);
  shownCaptions = w.m_viewport->measurementCaptions();
  require(row("ΔX") == "0.000 mm" && row("ΔY") == "+5.000 mm" && row("ΔZ") == "+5.000 mm" && row("Point 1 (face centroid)") == "(95.000, 0.000, 5.000) mm"
          && shownCaptions.contains("ΔY +5.000 mm") && !shownCaptions.filter("ΔX").size(),
          QString("and back in world axes: Δ %1 %2 %3, the view's arrows %4").arg(row("ΔX"), row("ΔY"), row("ΔZ"), shownCaptions.join(" | ")));
  if (auto* minimum = modeButton(0)) minimum->click();
  w.cancelTool();
  // A component that lies as the world does (an imported file's root, mostly) offers no axes of its own.
  const int displayed = w.m_viewport->displayedCount();
  const std::string shelf = w.m_doc->run("component", {{"name", "Shelf"}}).value("id", "");
  w.m_doc->run("import_brep", {{"brep", brepText(cube)}, {"name", "Book"}, {"parent", shelf}});
  std::string bookId;
  for (const auto& id : w.m_doc->scene.all_bodies())
    if (w.m_doc->nodeName(id) == "Book") bookId = id;
  require(until([&w, displayed] { return w.m_viewport->displayedCount() > displayed && w.m_viewport->remainingBodies() == 0; }, 30000) && !bookId.empty(),
          "the book in the unmoved component is displayed");
  w.startTool("length");
  w.m_viewport->selectRefs({faceRef(bookId, capTop)});
  w.onViewportSelection();
  until([&w] { return !w.m_lastMeasure.is_null(); }, 20000);
  require(!w.m_lastMeasure.is_null() && !frameShown() && row("Coordinates in").isEmpty(), "a pick in a component at the world's placement offers no component axes");
  w.cancelTool();

  QCoreApplication::exit(all ? 0 : 2);
  return true;
}
