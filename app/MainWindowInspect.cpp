// Inspect: the guided measuring tools, the section plane from a face, design checks (print, interference).
#include "MainWindow.hpp"
#include "CheckPanel.hpp"

#include <QKeyEvent>
#include <QStackedWidget>
#include <QStatusBar>

#include <cmath>
#include <map>
#include <memory>
#include <tuple>
#include <utility>

#include <BRepAlgoAPI_Common.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>

#include "Drawing2D.hpp"
#include "I18n.hpp"
#include "Units.hpp"
#include "opad/checks.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"

void MainWindow::buildInspectActions() {
  addAction("inspect.distance", tr("Distance"), "distance", QKeySequence("D"), [this] { toggleTool("distance"); }, true);
  addAction("inspect.angle", tr("Angle"), "angle", QKeySequence("A"), [this] { toggleTool("angle"); }, true);
  addAction("inspect.radius", tr("Radius"), "radius", QKeySequence("R"), [this] { toggleTool("radius"); }, true);
  addAction("inspect.bbox", tr("Bounding box"), "bbox", QKeySequence("B"), [this] { toggleTool("bbox"); }, true);
  addAction("inspect.area", tr("Area"), "area", QKeySequence(), [this] { toggleTool("area"); }, true);  // placed by the drawing2d area (UI-90)
  addAction("inspect.interference", tr("Interference"), "interference", QKeySequence(), [this] { startCheck(false); });
  addAction("inspect.printcheck", tr("Print check"), "printcheck", QKeySequence(), [this] { startCheck(true); });
  m_pinAction = addAction("inspect.pin", tr("Pin"), "pin", QKeySequence("P"), [this] { pinMeasurement(); });
  m_pinAction->setShortcutContext(Qt::ApplicationShortcut);
  m_pinAction->setEnabled(false);
  addAction("inspect.clear", tr("Clear measurement"), "", QKeySequence("Esc"), [this] {
    if (m_annotationEditor) return m_annotationEditor->cancel();
    if (m_design->sketchActive()) {  // the viewport did not have the focus: same as Esc in the sketch
      QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
      m_design->sketch()->sketchKey(&esc);
    } else if (m_design->escape()) {
    } else if (!m_tool.id.isEmpty()) toolEscape();
    else if (!closeTopPanel()) clearMeasurement();
  });  // Esc closes a tool panel first
  addAction("inspect.properties", tr("Properties"), "doc", QKeySequence("Ctrl+P"), [this] {
    if (m_selRefs.empty() && m_selRows.empty()) m_selRefs = m_viewport->selection();
    if (m_selRefs.empty() && m_selRows.empty()) throw opad::Error("Select something to see its properties.");
    showProperties(m_selRefs);
    openPanel(m_propsPanel);
  });
  addAction("select.geometry", tr("Select by geometry..."), "edges", QKeySequence(), [this] { selectGeometry(); });
  // Section is an inspection: it looks inside without changing anything.
  QAction* section = addAction("inspect.section", tr("Section"), "section", QKeySequence("X"), [this] {}, true);
  connect(section, &QAction::toggled, this, [this](bool on) {
    m_section->setEnabled(on);
    if (on) openPanel(m_sectionPanel);
    else m_sectionPanel->hide();
  });
  addAction("inspect.flip", tr("Flip section"), "flip", QKeySequence("Shift+X"), [this] { m_section->flip(); });
}

// ---------------------------------------------------------------- inspect (F23)
void MainWindow::sectionFromFace(const opad::Ref& face) {
  try {
    opad::json info = opad::inspect_ref(m_doc->doc, m_doc->scene, face);
    if (!info.contains("normal") || !info.contains("center")) {
      if (trace::enabled()) trace::log(QStringLiteral("section from face: not planar (%1)").arg(QString::fromStdString(info.value("surface", "?"))));
      statusBar()->showMessage(tr("Section: that face is %1; pick a planar face").arg(QString::fromStdString(info.value("surface", "not planar"))), 5000);
      return;
    }
    const opad::Vec3 o{info["center"][0].get<double>(), info["center"][1].get<double>(), info["center"][2].get<double>()};
    const opad::Vec3 n{info["normal"][0].get<double>(), info["normal"][1].get<double>(), info["normal"][2].get<double>()};
    if (trace::enabled()) trace::log(QStringLiteral("section from face: origin %1 %2 %3 normal %4 %5 %6").arg(o[0]).arg(o[1]).arg(o[2]).arg(n[0]).arg(n[1]).arg(n[2]));
    m_section->setFromFace(o, n);
    m_section->setEnabled(true);
    statusBar()->showMessage(tr("Section plane set from the picked face (Shift+X flips it)"), 5000);
  } catch (const std::exception& e) {
    if (trace::enabled()) trace::log(QStringLiteral("section from face failed: %1").arg(QString::fromUtf8(e.what())));
    statusBar()->showMessage(QString::fromUtf8(e.what()), 5000);
  }
}

// ---------------------------------------------------------------- guided tools
// Start the tool, then pick: the prompt bar and the tool panel walk through the steps. The viewport accumulates
// clicks while a tool runs, so its selection *is* the ordered pick list; everything here follows from
// toolPicksChanged(). A selection made before the tool was started is taken as its first picks.
QString MainWindow::refLabel(const opad::Ref& r) const {
  if (r.kind == opad::Ref::Kind::Point) return tr("Point %1").arg(units::vector(units::Kind::Length, r.point));  // a snapped or tracked point
  QString t = m_doc->nodeName(r.body);
  if (r.kind != opad::Ref::Kind::Body) t += QString::fromUtf8(" › %1 %2").arg(i18n::t(opad::Ref::kind_name(r.kind))).arg(r.index);
  return t;
}

QList<ToolStep> MainWindow::toolSteps() const {
  const Viewport::SelFilter f = m_viewport->selectionFilter();
  if (!m_tool.steps) {  // an open tool (Area): one step, the picks so far as what it got
    ToolStep s;
    s.label = f == Viewport::SelFilter::Vertex ? tr("Select points around the area") : f == Viewport::SelFilter::Face ? tr("Select fills or faces")
                                                                                          : tr("Select a closed object or the objects around an area");
    if (m_toolPicks.size() == 1) s.picked = refLabel(m_toolPicks.front());
    else if (!m_toolPicks.empty()) s.picked = tr("%1 picked").arg(m_toolPicks.size());
    return {s};
  }
  const QString kind = f == Viewport::SelFilter::Vertex && m_tool.id != "sectionface"
      ? (m_tool.id == "radius" ? tr("circle center") : tr("vertex or center"))
      : i18n::t(m_tool.id == "sectionface" || f == Viewport::SelFilter::Face ? "face" : f == Viewport::SelFilter::Edge ? "edge" : "body");
  QList<ToolStep> steps;
  for (int i = 0; i < m_tool.steps; ++i) {
    ToolStep s;
    s.label = m_tool.id == "sectionface" ? tr("Select a planar face") : m_tool.steps == 1 ? tr("Select a %1").arg(kind) : i == 0 ? tr("Select first %1").arg(kind) : tr("Select second %1").arg(kind);
    if (i < static_cast<int>(m_toolPicks.size())) s.picked = refLabel(m_toolPicks[i]);
    steps << s;
  }
  return steps;
}

void MainWindow::toggleTool(const QString& id) {
  if (m_tool.id == id) cancelTool();
  else startTool(id);
}

void MainWindow::startTool(const QString& id) {
  if (!m_doc->hasDocument || m_design->sketchActive()) return;
  if (m_annotationEditor) m_annotationEditor->cancel();  // one guide at a time
  m_design->escape();  // a feature panel or a plane pick gives way
  if (!m_tool.id.isEmpty()) cancelTool();
  if (m_toolStack->currentWidget() == m_checks) endCheck();
  m_toolStack->setCurrentWidget(m_toolSteps);
  static const std::map<QString, std::tuple<const char*, const char*, int>> kTools = {
      {"distance", {QT_TR_NOOP("Distance"), "distance", 2}}, {"angle", {QT_TR_NOOP("Angle"), "angle", 2}},       {"radius", {QT_TR_NOOP("Radius"), "radius", 1}},
      {"bbox", {QT_TR_NOOP("Bounding box"), "bbox", 1}},     {"sectionface", {QT_TR_NOOP("Section"), "section", 1}}, {"area", {QT_TR_NOOP("Area"), "area", 0}}};
  const auto it = kTools.find(id);
  if (it == kTools.end()) return;
  m_tool = Tool{id, tr(std::get<0>(it->second)), std::get<1>(it->second), std::get<2>(it->second)};
  m_toolPicks.clear();
  m_toolPoints.clear();
  m_toolHover.clear();
  ++m_toolRun;
  // Angles need faces/edges; radii also accept discovered centers. The section plane needs a face.
  const Viewport::SelFilter f = m_viewport->selectionFilter();
  const bool wantFaces = id == "sectionface" ? f != Viewport::SelFilter::Face : ((id == "angle" || id == "radius") && f == Viewport::SelFilter::Body) || (id == "angle" && f == Viewport::SelFilter::Vertex);
  // Area takes fills or faces, objects or points, never bodies: a drawing's objects, a solid's faces.
  const bool wantEdges = id == "area" && f == Viewport::SelFilter::Body && drawing2d::drawingOnly(m_doc->scene);
  m_viewport->setPickAccumulate(true, id == "distance");
  for (const char* a : {"inspect.distance", "inspect.angle", "inspect.radius", "inspect.bbox", "inspect.area"})
    action(a)->setChecked(id == QString(a).section('.', 1));
  if (toolMeasures()) {
    m_toolPanel->setHeader(m_tool.icon, m_tool.title);
    openPanel(m_toolPanel);
  }
  if (wantEdges) {
    action("select.edges")->trigger();
  } else if (wantFaces || (id == "area" && f == Viewport::SelFilter::Body)) {
    action("select.faces")->trigger();  // clears the picks and refreshes the prompt (see the select actions)
  } else {
    const auto before = m_viewport->selection();  // selected first, tool second still works
    if (!before.empty() && (!m_tool.steps || static_cast<int>(before.size()) <= m_tool.steps)) toolPicksChanged(before, false);
    else if (!before.empty()) m_viewport->clearSelection();
  }
  if (!m_tool.id.isEmpty()) refreshToolUi();
}

void MainWindow::cancelTool() {
  if (m_tool.id.isEmpty()) return;
  m_tool = Tool();  // first: hiding the panel below reports back here
  ++m_toolRun;
  if (Job* old = std::exchange(m_measureJob, nullptr)) old->cancel();
  m_toolPicks.clear();
  m_toolPoints.clear();
  for (const char* a : {"inspect.distance", "inspect.angle", "inspect.radius", "inspect.bbox", "inspect.area"}) action(a)->setChecked(false);
  m_viewport->setPickAccumulate(false);
  m_prompt->hide();
  m_toolPanel->hide();
  clearMeasurement();
}

void MainWindow::toolEscape() {
  if (!m_lastMeasure.is_null()) m_viewport->clearSelection();  // a result is showing: clear it and measure again
  else if (!m_toolPicks.empty()) m_viewport->deselectLast();   // one step back
  else cancelTool();
}

void MainWindow::toolPicksChanged(const std::vector<opad::Ref>& refs, bool fromClick) {
  if (m_tool.id == "sectionface") {  // the pick itself is handled with the section panel (sectionFromFace)
    if (!refs.empty()) cancelTool();
    return;
  }
  std::vector<opad::Ref> picks = refs;
  if (m_tool.steps && static_cast<int>(picks.size()) > m_tool.steps) {
    if (fromClick && static_cast<int>(m_toolPicks.size()) == m_tool.steps) return m_viewport->keepLastSelected();  // a pick after the last step starts over
    picks.resize(m_tool.steps);  // a rubber band caught more than the tool asks for
  }
  // A click that changed nothing (on empty space: XOR keeps the picks) must not throw the result away and measure again.
  if (fromClick && std::equal(picks.begin(), picks.end(), m_toolPicks.begin(), m_toolPicks.end(), [](const opad::Ref& a, const opad::Ref& b) { return a.str() == b.str(); })) return;
  opad::Vec3 at{0, 0, 0};
  const bool hasPoint = fromClick && picks.size() == m_toolPoints.size() + 1 && m_viewport->lastPickPoint(at);
  if (picks.size() > m_toolPoints.size()) {
    while (m_toolPoints.size() + 1 < picks.size()) m_toolPoints.push_back({false, at});
    m_toolPoints.push_back({hasPoint, at});
  } else {
    m_toolPoints.resize(picks.size());
  }
  m_toolPicks = picks;
  ++m_toolRun;
  if (Job* old = std::exchange(m_measureJob, nullptr)) old->cancel();  // a superseded measure must stop computing, not just be ignored
  m_lastMeasure = opad::json();
  m_pinAction->setEnabled(false);
  m_viewport->clearDimension();
  m_viewport->clearPreview();
  m_viewport->setMeasurementSelectionLocked(m_tool.id == "distance" && picks.size() == 2
      && picks[0].kind == opad::Ref::Kind::Edge && picks[1].kind == opad::Ref::Kind::Edge);
  std::vector<opad::Vec3> marks;
  for (const auto& p : m_toolPoints) if (p.first) marks.push_back(p.second);
  m_viewport->showPickMarkers(marks);
  if (m_tool.steps ? static_cast<int>(picks.size()) == m_tool.steps : !picks.empty()) runToolMeasure();  // an open tool: after every pick
  refreshToolUi();
}

// The measurement is exact geometry (BRepExtrema, BRepGProp): on a worker, never in the click handler.
void MainWindow::runToolMeasure() {
  const std::vector<opad::Ref> refs = m_toolPicks;
  const auto pickedPoints = m_toolPoints;
  const double snapTolerance = m_viewport->pixelSize() * 14.0;
  const int run = m_toolRun;
  auto result = std::make_shared<opad::json>();
  const QString kind = m_tool.id;
  // Straight to the measure functions with the app's resolved scene; the "measure" command would resolve the
  // whole scene from the op log again on every call.
  if (Job* old = std::exchange(m_measureJob, nullptr)) old->cancel();
  auto document = std::make_shared<opad::Document>(m_doc->doc);
  auto scene = std::make_shared<opad::Scene>(m_doc->scene);
  m_measureJob = m_jobs->async(tr("Measuring %1").arg(m_tool.title), [document, scene, refs, pickedPoints, snapTolerance, kind, result](Progress progress) {
    if (kind == "distance" && refs.at(0).kind == opad::Ref::Kind::Edge && refs.at(1).kind == opad::Ref::Kind::Edge) {
      const bool clicked = pickedPoints.size() >= 2 && pickedPoints[0].first && pickedPoints[1].first;
      opad::json closest;
      if (!clicked) closest = opad::measure_distance(*document, *scene, refs[0], refs[1], [progress] { return progress.cancelled(); });
      *result = opad::measure_edge_distance(*document, *scene, refs[0], refs[1],
          clicked ? pickedPoints[0].second : closest["point_a"].get<opad::Vec3>(),
          clicked ? pickedPoints[1].second : closest["point_b"].get<opad::Vec3>(),
          clicked ? snapTolerance : 0.0, [progress] { return progress.cancelled(); });
    }
    else if (kind == "distance") *result = opad::measure_distance(*document, *scene, refs.at(0), refs.at(1), [progress] { return progress.cancelled(); });
    else if (kind == "angle") *result = opad::measure_angle(*document, *scene, refs.at(0), refs.at(1));
    else if (kind == "radius") *result = opad::measure_radius(*document, *scene, refs.at(0));
    else if (kind == "area") *result = opad::measure_area(*document, *scene, refs, [progress] { return progress.cancelled(); });
    else *result = opad::measure_bbox(*document, *scene, refs);
  }, [this, run, result](bool ok, const QString& error) {
    if (run == m_toolRun) m_measureJob = nullptr;
    if (run != m_toolRun || m_tool.id.isEmpty()) return;  // the picks moved on
    if (!ok) {
      statusBar()->showMessage(i18n::t(error), 6000);
      return m_viewport->deselectLast();  // that pick does not work for this tool: ask for it again
    }
    m_lastMeasure = *result;
    m_pinAction->setEnabled(!m_doc->browse && m_lastMeasure.value("closed", true));
    m_viewport->showMeasurement(m_lastMeasure);
    refreshToolUi();
  });
}

void MainWindow::refreshToolUi() {
  if (m_tool.id.isEmpty()) return;
  const QList<ToolStep> steps = toolSteps();
  const int picked = static_cast<int>(m_toolPicks.size());
  const bool open = m_tool.id == "area" && !m_lastMeasure.is_null() && !m_lastMeasure.value("closed", false);  // picked so far encloses nothing
  const bool done = !m_lastMeasure.is_null() && !open;
  const QString hints = m_tool.id == "area" ? (done ? (m_doc->browse ? tr("Click to add · Esc back · 1–4 filter") : tr("Click to add · P pin · Esc back")) : picked ? tr("Click to add · Esc back · 1–4 filter") : tr("Esc cancel · 1–4 change filter"))
      : m_viewport->selectionFilter() == Viewport::SelFilter::Vertex && !done ? tr("Ctrl-click arc to select center · Esc back") : done ? (m_doc->browse ? tr("Esc clear · 1–4 filter") : tr("P pin · Esc clear · 1–4 filter")) : picked ? tr("Esc back · 1–4 change filter") : tr("Esc cancel · 1–4 change filter");
  m_prompt->set(m_tool.icon, m_tool.title, steps, hints);
  m_prompt->show();
  positionOverlays();
  if (!toolMeasures()) return;

  const Viewport::SelFilter f = m_viewport->selectionFilter();
  // As the filter is named now: a drawing's objects and points in 2D (UI-118).
  const QString kinds = action(f == Viewport::SelFilter::Face ? "select.faces" : f == Viewport::SelFilter::Edge ? "select.edges" : f == Viewport::SelFilter::Vertex ? "select.vertices" : "select.bodies")->text().remove('&');
  m_toolPanel->setContext(m_tool.steps ? tr("%1 · %2 of %3").arg(kinds.toLower()).arg(picked).arg(m_tool.steps) : tr("%1 · %2 picked").arg(kinds.toLower()).arg(picked));
  m_toolSteps->setSteps(steps, m_toolHover);
  const QString waiting = picked < steps.size() ? steps[picked].label : tr("Measuring…");
  QString explanation = waiting;
  if (!m_tool.steps && picked && m_lastMeasure.is_null()) explanation = tr("Measuring…");
  if (open) {
    const int ends = m_lastMeasure.value("open_ends", 0);
    explanation = m_lastMeasure.value("points", 0) ? tr("Pick at least three points: the area closes back to the first.")
                  : ends ? tr("Not closed yet: loose ends %1. Pick the objects that close it.").arg(ends)
                         : tr("These objects enclose nothing. Pick a closed object, or every object around the area.");
  } else if (done && m_tool.id == "area") {
    explanation = m_lastMeasure.value("grown", false) ? tr("The smallest area the picked object closes with the objects it meets. Pick more to add them.")
                  : m_lastMeasure.value("points", 0) ? tr("The polygon through the picked points, closed back to the first.")
                                                     : tr("The area inside the picked boundary; areas inside it are holes.");
  } else if (done) {
    if (m_tool.id == "distance" && m_lastMeasure.contains("anchors")) explanation = tr("Click an anchor marker to move that measurement point. Edges stay selected until Esc or Clear. Choose a preset pair below.");
    else if (m_tool.id == "distance") explanation = tr("Shortest distance between the selections. Δ = point 2 − point 1 in world axes.");
    else if (m_tool.id == "angle") explanation = tr("Directions compared at a common origin. Planar faces use their normals; curved faces use their axes.");
    else if (m_tool.id == "radius") explanation = tr("Radius from the center or cylinder axis to the surface.");
    else explanation = tr("Bounding box aligned with the world X, Y and Z axes.");
  }
  m_toolSteps->setSummary(m_tool.title, explanation, open ? tr("open") : done && !m_doc->browse ? tr("unpinned") : QString());
  QStringList anchorLabels;
  int anchorIndex = 0;
  if (done && m_lastMeasure.contains("anchors")) {
    anchorIndex = m_lastMeasure.value("anchor_index", 0);
    for (const auto& anchor : m_lastMeasure["anchors"]) {
      const std::string key = anchor.value("kind", "picked");
      if (key == "picked") anchorLabels << tr("Picked points");
      else if (key == "custom") anchorLabels << tr("Selected anchors");
      else if (key == "closest") anchorLabels << tr("Closest points");
      else if (key == "farthest") anchorLabels << tr("Farthest points");
      else if (key == "edge1_start") anchorLabels << tr("Edge 1 start → nearest");
      else if (key == "edge1_end") anchorLabels << tr("Edge 1 end → nearest");
      else if (key == "edge1_midpoint") anchorLabels << tr("Edge 1 midpoint → nearest");
      else if (key == "edge1_quarter") anchorLabels << tr("Edge 1 quarter → nearest");
      else if (key == "edge1_three_quarter") anchorLabels << tr("Edge 1 three-quarter → nearest");
      else if (key == "edge2_start") anchorLabels << tr("Nearest → edge 2 start");
      else if (key == "edge2_end") anchorLabels << tr("Nearest → edge 2 end");
      else if (key == "edge2_midpoint") anchorLabels << tr("Nearest → edge 2 midpoint");
      else if (key == "edge2_quarter") anchorLabels << tr("Nearest → edge 2 quarter");
      else if (key == "edge2_three_quarter") anchorLabels << tr("Nearest → edge 2 three-quarter");
      else anchorLabels << tr("Curve points of interest");
    }
  }
  m_toolSteps->setAnchorOptions(anchorLabels, anchorIndex);
  m_toolSteps->setComponentsState(done && m_viewport->measurementHasMultipleAxes(), m_viewport->measurementComponents());
  QList<QPair<QString, QString>> rows;
  if (m_tool.id == "area") {  // the area and its perimeter; while open, how long the picks are
    if (!m_lastMeasure.is_null()) {
      const opad::json& r = m_lastMeasure;
      if (done) rows << qMakePair(m_tool.title, units::format(units::Kind::Area, r.value("value", 0.0)));
      rows << qMakePair(done ? tr("Perimeter") : tr("Length"), units::format(units::Kind::Length, r.value("perimeter", 0.0)));
      if (r.value("loops", 0) > 1) rows << qMakePair(tr("Loops"), QString::number(r.value("loops", 0)));
      if (r.value("holes", 0) > 0) rows << qMakePair(tr("Holes"), QString::number(r.value("holes", 0)));
      if (r.value("grown", false)) rows << qMakePair(tr("Objects around it"), QString::number(r.value("edges", 0)));
    }
    m_toolSteps->setResult(rows);
    m_toolSteps->setFooter(done, !m_doc->browse);
    return;
  }
  if (done) {
    const opad::json& r = m_lastMeasure;
    // Results come in mm and degrees; they are shown in the document's unit and precision (UI-123).
    const units::Kind kind = r.value("unit", "mm") == "deg" ? units::Kind::Angle : units::Kind::Length;
    if (r.contains("value")) rows << qMakePair(m_tool.title, units::format(kind, r["value"].get<double>()));
    if (r.contains("delta"))
      for (int i = 0; i < 3; ++i) {
        const double d = r["delta"][i].get<double>();
        const bool plus = d > 0 && units::number(units::Kind::Length, d) != units::number(units::Kind::Length, 0);
        rows << qMakePair(tr("Δ%1").arg(QChar("XYZ"[i])), (plus ? "+" : "") + units::format(units::Kind::Length, d));
      }
    if (r.contains("supplement")) rows << qMakePair(tr("Supplement"), units::format(units::Kind::Angle, r["supplement"].get<double>()));
    if (r.contains("diameter")) rows << qMakePair(tr("Diameter"), units::format(units::Kind::Length, r["diameter"].get<double>()));
    for (const char* k : {"size", "min", "max"})
      if (r.contains(k) && r[k].is_array() && r[k].size() == 3) rows << qMakePair(i18n::t(QString("bbox %1").arg(k)), units::vector(units::Kind::Length, r[k].get<std::array<double, 3>>()));
    if (r.contains("relation") && r["relation"].is_string()) rows << qMakePair(tr("Relation"), i18n::t(QString::fromStdString(r["relation"].get<std::string>())));
  }
  for(size_t i=0;i<m_toolPicks.size();++i) {
    const auto info=m_viewport->circleInfo(m_toolPicks[i]);
    if(info.contains("diameter")) rows << qMakePair(tr("Circle %1 diameter").arg(i+1),units::format(units::Kind::Length,info["diameter"].get<double>()));
    if(info.contains("segments")) rows << qMakePair(tr("Circle %1 mesh segments (approximate)").arg(i+1),QString::number(info["segments"].get<int>()));
  }
  m_toolSteps->setResult(rows);
  m_toolSteps->setFooter(done, !m_doc->browse);
}

void MainWindow::pinMeasurement() {
  if (m_lastMeasure.is_null()) return;
  if (!requireEditable([this] { pinMeasurement(); })) return;  // a pinned measurement is part of the document
  opad::json op;
  op["op"] = "measurement";
  op["kind"] = m_lastMeasure.value("kind", "distance");
  opad::json refs = opad::json::array();
  for (const auto& r : m_lastMeasure.value("refs", opad::json::array())) refs.push_back(opad::Ref::parse(r.get<std::string>()).to_json());
  op["refs"] = refs;
  op["result"] = m_lastMeasure;
  opad::json r = m_doc->run("append", opad::json{{"op", op}});
  if (r.contains("appended") && !r["appended"].empty()) m_timeline->setCurrentOp(r["appended"][0].get<std::string>());
  statusBar()->showMessage(tr("Measurement pinned. Manage it in Annotations (Alt+2)."), 4000);
  if (!m_tool.id.isEmpty()) m_viewport->clearSelection();  // the tool stays on for the next measurement
}

void MainWindow::clearMeasurement() {
  m_lastMeasure = opad::json();
  m_viewport->clearDimension();
  m_pinAction->setEnabled(false);
  m_viewport->clearSelection();
}

// ---------------------------------------------------------------- design checks (TODO 10 B13, B17)
void MainWindow::startCheck(bool print) {
  if (!m_doc->hasDocument) return;
  if (m_annotationEditor) m_annotationEditor->cancel();
  m_design->escape();
  if (!m_tool.id.isEmpty()) cancelTool();
  m_checkSelect = currentNodeIds();  // the selection when the check starts; clicking findings changes it later
  m_checks->begin(print ? CheckPanel::Mode::Print : CheckPanel::Mode::Interference);
  m_toolStack->setCurrentWidget(m_checks);
  m_toolPanel->setHeader(print ? "printcheck" : "interference", print ? tr("Print check") : tr("Interference"));
  openPanel(m_toolPanel);
  runCheck();
}

void MainWindow::runCheck() {
  if (Job* old = std::exchange(m_checkJob, nullptr)) old->cancel();
  opad::json args = m_checks->options();
  if (!m_checkSelect.empty()) args["select"] = m_checkSelect;
  args["limit"] = 200;
  const bool print = m_checks->mode() == CheckPanel::Mode::Print;
  auto document = std::make_shared<opad::Document>(m_doc->doc);
  auto scene = std::make_shared<opad::Scene>(m_doc->scene);
  auto result = std::make_shared<opad::json>();
  m_checks->setRunning(tr("Checking…"));
  m_checkJob = m_jobs->async(print ? tr("Print check") : tr("Interference"), [document, scene, args, print, result](Progress p) {
    const auto cancelled = [p] { return p.cancelled(); };
    *result = print ? opad::check_print(*document, *scene, args, cancelled) : opad::check_interference(*document, *scene, args, cancelled);
  }, [this, result](bool ok, const QString& error) {
    m_checkJob = nullptr;
    if (!ok) return m_checks->setFailed(error == "cancelled" ? tr("Cancelled.") : i18n::t(error));
    m_checks->setResult(*result);
  });
}

void MainWindow::showFinding(const opad::json& f) {
  m_viewport->clearPreviewBodies();
  if (f.contains("a")) {  // a pair of bodies
    const std::string a = f.value("a", ""), b = f.value("b", "");
    m_viewport->selectNodes({a, b});
    if (f.value("kind", "") == "clearance" && f.contains("point_a")) {
      m_viewport->showMeasurement({{"kind", "distance"}, {"value", f.value("distance_mm", 0.0)}, {"unit", "mm"}, {"point_a", f["point_a"]}, {"point_b", f["point_b"]}});
      return;
    }
    // The overlap itself, shown as a preview body: computed again on a worker (the check keeps its volume and box).
    if (Job* old = std::exchange(m_overlapJob, nullptr)) old->cancel();
    auto document = std::make_shared<opad::Document>(m_doc->doc);
    auto scene = std::make_shared<opad::Scene>(m_doc->scene);
    auto shape = std::make_shared<TopoDS_Shape>();
    auto prs = std::make_shared<std::shared_ptr<const BodyPrs>>();
    m_overlapJob = m_jobs->async(tr("Showing the overlap"), [document, scene, a, b, shape, prs](Progress) {
      BRepAlgoAPI_Common common(opad::node_world_shape(*document, *scene, a), opad::node_world_shape(*document, *scene, b));
      if (!common.IsDone()) return;
      *shape = common.Shape();
      BodyPrs::meshForDisplay(*shape, 0.05);
      Bnd_Box box;
      BRepBndLib::Add(*shape, box, Standard_False);
      *prs = BodyPrs::build(*shape, box, true);
    }, [this, shape, prs](bool ok, const QString&) {
      m_overlapJob = nullptr;
      if (ok && !shape->IsNull() && m_toolStack->currentWidget() == m_checks) m_viewport->setPreviewBodies(std::vector<Viewport::PreviewPart>{{std::string(), *shape, *prs}}, {});
    });
    return;
  }
  // A print finding: the body's faces, highlighted as a face selection.
  const std::string body = f.value("body", "");
  std::vector<opad::Ref> refs;
  for (const auto& i : f.value("faces", opad::json::array())) {
    opad::Ref r;
    r.body = body;
    r.kind = opad::Ref::Kind::Face;
    r.index = i.get<int>();
    refs.push_back(r);
  }
  if (refs.empty()) return m_viewport->selectNodes({body});
  if (m_viewport->selectionFilter() == Viewport::SelFilter::Face) return m_viewport->selectRefs(refs);
  connect(m_viewport, &Viewport::filterApplied, this, [this, refs] { m_viewport->selectRefs(refs); }, Qt::SingleShotConnection);
  m_viewport->setSelectionFilter(Viewport::SelFilter::Face);
}

void MainWindow::endCheck() {
  if (Job* old = std::exchange(m_checkJob, nullptr)) old->cancel();
  if (Job* old = std::exchange(m_overlapJob, nullptr)) old->cancel();
  m_viewport->clearPreviewBodies();
}
