// Inspect: the guided measuring tools, the section plane from a face, design checks (print, interference).
#include "MainWindow.hpp"
#include "CheckPanel.hpp"

#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QRegularExpression>
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
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include "Drawing2D.hpp"
#include "CommandHelp.hpp"
#include "HelpClip.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "KeyText.hpp"
#include "Units.hpp"
#include "opad/checks.hpp"
#include "opad/explode.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"

namespace {
// Why a pick cannot be measured, as the core says it, translated (UI-50): the radius' "not a <type> <kind>" (what was
// picked) is made at run time, so it is split off and its words translated on their own.
QString measureReason(const QString& why) {
  static const QRegularExpression found(QStringLiteral(", not a (\\w+) (\\w+)\\)$"));
  const QRegularExpressionMatch m = found.match(why);
  if (!m.hasMatch()) return i18n::t(why);
  return MainWindow::tr("%1; it is a %2 %3").arg(i18n::t(why.left(m.capturedStart()) + ")"), i18n::t(m.captured(1)), i18n::t(m.captured(2)));
}
}  // namespace

OPAD_ICON_TABLE(measure, {"length", R"(<path d="M3 16.5L16.5 3L21 7.5L7.5 21z"/><path d="M7.5 12l2 2M10.5 9l2 2M13.5 6l2 2"/>)"});

void MainWindow::buildInspectActions() {
  addAction("inspect.distance", tr("Distance"), "distance", QKeySequence("D"), [this] { toggleTool("distance"); }, true);
  addAction("inspect.angle", tr("Angle"), "angle", QKeySequence("A"), [this] { toggleTool("angle"); }, true);
  addAction("inspect.radius", tr("Radius"), "radius", QKeySequence("R"), [this] { toggleTool("radius"); }, true);
  addAction("inspect.bbox", tr("Bounding box"), "bbox", QKeySequence("B"), [this] { toggleTool("bbox"); }, true);
  addAction("inspect.area", tr("Area"), "area", QKeySequence(), [this] { toggleTool("area"); }, true);  // placed by the drawing2d area (UI-90)
  addAction("inspect.length", tr("Length and area"), "length", QKeySequence("Shift+L"), [this] { toggleTool("length"); }, true);
  addAction("inspect.interference", tr("Interference"), "interference", QKeySequence(), [this] { startCheck(false); });
  addAction("inspect.printcheck", tr("Print check"), "printcheck", QKeySequence(), [this] { startCheck(true); });
  m_pinAction = addAction("inspect.pin", tr("Pin"), "pin", QKeySequence("P"), [this] { pinMeasurement(); });
  m_distanceMode = std::clamp(m_settings.value("measure/distanceMode", 0).toInt(), 0, 2);
  m_measureFrame = std::clamp(m_settings.value("measure/frame", 0).toInt(), 0, 1);
  m_pinAction->setShortcutContext(Qt::ApplicationShortcut);
  m_pinAction->setEnabled(false);
  CommandInfo clear;  // Esc is fixed: the key every footer, prompt and tool panel names for stepping back (TODO 11 wave 3)
  clear.id = "inspect.clear";
  clear.label = tr("Clear measurement");
  clear.key = QKeySequence("Esc");
  clear.fixedKey = true;
  addCommand(clear, [this] {
    if (m_annotationEditor) return m_annotationEditor->cancel();
    if (m_design->sketchActive()) {  // the viewport did not have the focus: same as Esc in the sketch
      QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
      m_design->sketch()->sketchKey(&esc);
    } else if (m_design->escape()) {
    } else if (!m_tool.id.isEmpty()) toolEscape();
    else if (!closeTopPanel()) clearMeasurement();
  });  // Esc closes a tool panel first
  addAction("inspect.properties", tr("Properties"), "doc", QKeySequence("Alt+Return"), [this] {  // Ctrl+P is Print's (UI-111)
    if (m_selRefs.empty() && m_selRows.empty()) m_selRefs = m_viewport->selection();
    if (m_selRefs.empty() && m_selRows.empty()) throw opad::UserHint("Select something to see its properties.", true);
    showProperties(m_selRefs);
    openPanel(m_propsPanel);
  });
  // Section is an inspection: it looks inside without changing anything.
  QAction* section = addAction("inspect.section", tr("Section"), "section", QKeySequence("X"), [this] {}, true);
  connect(section, &QAction::toggled, this, [this](bool on) {
    m_section->setEnabled(on);
    if (on) openPanel(m_sectionPanel);
    else m_sectionPanel->hide();
  });
  addAction("inspect.flip", tr("Flip section"), "flip", QKeySequence("Shift+X"), [this] { m_section->flip(); });
  connect(keys::notifier(), &keys::Notifier::changed, this, [this] { refreshToolUi(); });  // the prompt names keys
}

// ---------------------------------------------------------------- inspect (F23)
// The face is inspected on a worker (UI-51: inspect_ref walks the body); the plane is set when it answers.
void MainWindow::sectionFromFace(const opad::Ref& face) {
  if (Job* old = std::exchange(m_sectionJob, nullptr)) old->cancel();
  auto document = m_doc->shapesOf({face.body});
  auto scene = std::make_shared<opad::Scene>(m_doc->scene);
  auto result = std::make_shared<opad::json>();
  const auto generation = m_doc->generation;
  m_sectionJob = m_jobs->async(tr("Section from the picked face"), [document, scene, face, result](Progress) {
    *result = opad::inspect_ref(*document, *scene, face);
  }, [this, face, result, generation](bool ok, const QString& error) {
    m_sectionJob = nullptr;
    if (generation != m_doc->generation || error == "cancelled") return;
    if (!ok) {
      if (trace::enabled()) trace::log(QStringLiteral("section from face failed: %1").arg(error));
      return hint(error, false);
    }
    const opad::json& info = *result;
    if (!info.contains("normal") || !info.contains("center")) {
      if (trace::enabled()) trace::log(QStringLiteral("section from face: not planar (%1)").arg(QString::fromStdString(info.value("surface", "?"))));
      hint(tr("Section: that face is %1; pick a planar face").arg(QString::fromStdString(info.value("surface", "not planar"))), false);
      return;
    }
    const opad::Vec3 lift = m_viewport->shownOffset(face.body);  // an exploded part: the plane goes where the face is drawn
    const opad::Vec3 o{info["center"][0].get<double>() + lift[0], info["center"][1].get<double>() + lift[1], info["center"][2].get<double>() + lift[2]};
    const opad::Vec3 n{info["normal"][0].get<double>(), info["normal"][1].get<double>(), info["normal"][2].get<double>()};
    if (trace::enabled()) trace::log(QStringLiteral("section from face: origin %1 %2 %3 normal %4 %5 %6").arg(o[0]).arg(o[1]).arg(o[2]).arg(n[0]).arg(n[1]).arg(n[2]));
    m_section->setFromFace(o, n);
    m_section->setEnabled(true);
    m_toasts->toast(help::expand(tr("Section plane set from the picked face; Flip section ({key:inspect.flip}) turns it over")), tr("Flip"), [this] { m_section->flip(); });
  });
}

// ---------------------------------------------------------------- guided tools
// Start the tool, then pick: the prompt bar and the tool panel walk through the steps. The viewport accumulates
// clicks while a tool runs, so its selection *is* the ordered pick list; everything here follows from
// toolPicksChanged(). A selection made before the tool was started is taken as its first picks.
QString MainWindow::refLabel(const opad::Ref& r) const {
  if (r.kind == opad::Ref::Kind::Point) return tr("Point %1").arg(units::vector(units::Kind::Length, r.point));  // a snapped or tracked point
  QString t = m_doc->nodeName(r.body);
  const opad::Node* n = m_doc->node(r.body);
  const bool drawing = m_viewport->drawingWords() && n && n->representation == "drawing2d";  // "Lines › object 3" (UI-118)
  if (r.kind != opad::Ref::Kind::Body) t += QString::fromUtf8(" › %1 %2").arg(i18n::t(drawing ? drawing2d::kindWord(r.kind) : opad::Ref::kind_name(r.kind))).arg(r.index);
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
  const bool words2d = m_viewport->drawingWords() && drawing2d::hasDrawings(m_doc->scene);  // as the filters are named then (UI-118)
  const QString kind = f == Viewport::SelFilter::Vertex && m_tool.id != "sectionface"
      ? (m_tool.id == "radius" ? tr("circle center") : words2d ? tr("point or center") : tr("vertex or center"))
      : i18n::t(m_tool.id == "sectionface" ? "face" : f == Viewport::SelFilter::Face ? (words2d ? "fill" : "face") : f == Viewport::SelFilter::Edge ? (words2d ? "object" : "edge") : (words2d ? "group" : "body"));
  const QString one = words2d && f == Viewport::SelFilter::Edge ? tr("Select an object") : tr("Select a %1").arg(kind);
  QList<ToolStep> steps;
  for (int i = 0; i < m_tool.steps; ++i) {
    ToolStep s;
    s.label = m_tool.id == "sectionface" ? tr("Select a planar face") : m_tool.steps == 1 ? one : i == 0 ? tr("Select first %1").arg(kind) : tr("Select second %1").arg(kind);
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
      {"bbox", {QT_TR_NOOP("Bounding box"), "bbox", 1}},     {"sectionface", {QT_TR_NOOP("Section"), "section", 1}}, {"area", {QT_TR_NOOP("Area"), "area", 0}},
      {"length", {QT_TR_NOOP("Length and area"), "length", 1}}};
  const auto it = kTools.find(id);
  if (it == kTools.end()) return;
  m_tool = Tool{id, tr(std::get<0>(it->second)), std::get<1>(it->second), std::get<2>(it->second)};
  m_toolPicks.clear();
  m_toolPoints.clear();
  m_toolHover.clear();
  m_toolError.clear();
  ++m_toolRun;
  // Angles need faces/edges; radii also accept discovered centers. The section plane needs a face.
  const Viewport::SelFilter f = m_viewport->selectionFilter();
  const bool wantFaces = id == "sectionface" ? f != Viewport::SelFilter::Face : ((id == "angle" || id == "radius") && f == Viewport::SelFilter::Body) || (id == "angle" && f == Viewport::SelFilter::Vertex);
  // Area takes fills or faces, objects or points, never bodies: a drawing's objects, a solid's faces. In 2D words the Faces
  // filter is gone (the drawing2d area hides it): a drawing's objects instead (a hidden action still triggers). Length and
  // area takes edges, faces or bodies: not vertices.
  const bool noFaces = !action("select.faces")->isVisible() && id != "sectionface";
  const bool wantEdges = (id == "length" && f == Viewport::SelFilter::Vertex) ||
                         (id == "area" && f == Viewport::SelFilter::Body && drawing2d::drawingOnly(m_doc->scene)) ||
                         (noFaces && (wantFaces || (id == "area" && f == Viewport::SelFilter::Body)));
  m_viewport->setPickAccumulate(true, id == "distance");
  // Object snap where a free point is a pick; Radius takes a circle's centre (its marker), the section a face.
  m_viewport->setSnapPicks(id == "distance" || id == "bbox" || id == "area" ? Viewport::SnapPicks::Points : Viewport::SnapPicks::None);
  for (const char* a : {"inspect.distance", "inspect.angle", "inspect.radius", "inspect.bbox", "inspect.area", "inspect.length"})
    action(a)->setChecked(id == QString(a).section('.', 1));
  if (toolMeasures()) {
    m_toolPanel->setHeader(m_tool.icon, m_tool.title);
    m_toolSteps->setGuide("inspect." + id);  // UI-107
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
  m_toolError.clear();
  ++m_toolRun;
  if (Job* old = std::exchange(m_measureJob, nullptr)) old->cancel();
  m_toolPicks.clear();
  m_toolPoints.clear();
  m_viewport->setSnapFrom(std::nullopt);
  m_viewport->setSnapPicks(Viewport::SnapPicks::None);
  for (const char* a : {"inspect.distance", "inspect.angle", "inspect.radius", "inspect.bbox", "inspect.area", "inspect.length"}) action(a)->setChecked(false);
  m_viewport->setPickAccumulate(false);
  m_viewport->setMeasurementFrame(gp_Trsf());
  m_prompt->hide();
  m_toolPanel->hide();
  clearMeasurement();
}

void MainWindow::toolEscape() {
  if (!m_lastMeasure.is_null() && m_tool.steps) m_viewport->clearSelection();  // a result is showing: clear it and measure again (Area: one pick back)
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
  if (picks.size() > m_toolPicks.size()) m_toolError.clear();  // a new pick: the last one's error is over (a step back keeps it)
  m_toolPicks = picks;
  // Perpendicular and tangent snaps go from the point picked last (UI-90).
  m_viewport->setSnapFrom(picks.empty() ? std::nullopt : picks.back().kind == opad::Ref::Kind::Point ? std::optional(picks.back().point)
                          : m_toolPoints.back().first ? std::optional(m_toolPoints.back().second) : std::nullopt);
  ++m_toolRun;
  if (Job* old = std::exchange(m_measureJob, nullptr)) old->cancel();  // a superseded measure must stop computing, not just be ignored
  m_lastMeasure = opad::json();
  m_pinAction->setEnabled(false);
  m_viewport->clearDimension();
  m_viewport->clearPreview();
  m_viewport->setMeasurementSelectionLocked(m_tool.id == "distance" && m_distanceMode == 0 && picks.size() == 2
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
  const int mode = m_distanceMode;
  // Straight to the measure functions with the app's resolved scene; the "measure" command would resolve the
  // whole scene from the op log again on every call.
  if (Job* old = std::exchange(m_measureJob, nullptr)) old->cancel();
  std::vector<std::string> picked;
  for (const auto& r : refs) picked.push_back(r.body);
  auto document = m_doc->shapesOf(picked);
  auto scene = std::make_shared<opad::Scene>(m_doc->scene);
  const auto moved = m_viewport->shownOffsets();  // an exploded view: measured where the parts are drawn
  m_measureJob = m_jobs->async(tr("Measuring %1").arg(m_tool.title), [document, scene, moved, refs, pickedPoints, snapTolerance, kind, mode, result](Progress progress) {
    if (!moved.empty()) *scene = opad::exploded_scene(*scene, moved);
    if (kind == "distance" && mode == 1) *result = opad::measure_center_distance(*document, *scene, refs.at(0), refs.at(1));
    else if (kind == "distance" && mode == 2) *result = opad::measure_max_distance(*document, *scene, refs.at(0), refs.at(1), [progress] { return progress.cancelled(); });
    else if (kind == "distance" && refs.at(0).kind == opad::Ref::Kind::Edge && refs.at(1).kind == opad::Ref::Kind::Edge) {
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
    else if (kind == "area") *result = opad::measure_area(*document, *scene, refs, [progress] { return progress.cancelled(); },
                                                          pickedPoints.size() == 1 && pickedPoints[0].first ? std::optional(pickedPoints[0].second) : std::nullopt);
    else if (kind == "length") *result = opad::measure_length(*document, *scene, refs.at(0));
    else *result = opad::measure_bbox(*document, *scene, refs);
    if (!moved.empty()) (*result)["exploded"] = true;  // not pinned: a pinned measurement is the assembled model's
  }, [this, run, result](bool ok, const QString& error) {
    if (run == m_toolRun) m_measureJob = nullptr;
    if (run != m_toolRun || m_tool.id.isEmpty()) return;  // the picks moved on
    if (!ok) {  // that pick does not work for this tool: said in the panel (UI-50), and the pick is asked for again
      QString why = error;
      for (const auto& pick : m_toolPicks)  // the core names the reference by its id: the panel names the pick
        if (why.endsWith(": " + QString::fromStdString(pick.str()))) why.chop(static_cast<int>(pick.str().size()) + 2);
      m_toolError = tr("%1 cannot be measured: %2").arg(m_toolPicks.empty() ? tr("That pick") : refLabel(m_toolPicks.back()), measureReason(why));
      statusBar()->showMessage(m_toolError, 6000);
      m_viewport->deselectLast();
      return refreshToolUi();
    }
    m_lastMeasure = *result;
    // An area the picks do not close yet (UI-90) is neither kept among the earlier results nor pinned; a length's "closed"
    // says whether its edge is a loop.
    const bool unclosed = m_tool.id == "area" && !m_lastMeasure.value("closed", false);
    // Earlier results (UI-144): an open tool's (Area, after every pick) replaces its own until the picks start over.
    if (!unclosed) {
      const bool same = !m_tool.steps && m_toolPicks.size() > 1 && !m_measureHistory.empty() && m_measureHistory.front().result.value("kind", "") == m_lastMeasure.value("kind", "");
      if (same) m_measureHistory.front() = MeasureRecord{*result};
      else m_measureHistory.insert(m_measureHistory.begin(), MeasureRecord{*result});
      if (m_measureHistory.size() > 12) m_measureHistory.pop_back();
    }
    m_pinAction->setEnabled(!m_doc->browse && !measuredExploded() && !unclosed);
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
  // The keys as bound now (keys::notifier refreshes this); an entry whose command has no key is left out.
  const QString esc = keys::fixedText("esc"), filters = keys::span({"select.bodies", "select.faces", "select.edges", "select.vertices"}), pinKey = keys::text("inspect.pin");
  const QString click = tr("Click to add"), back = tr("%1 back").arg(esc), cancel = tr("%1 cancel").arg(esc), clear = tr("%1 clear").arg(esc);
  const QString filter = filters.isEmpty() ? QString() : tr("%1 filter").arg(filters), change = filters.isEmpty() ? QString() : tr("%1 change filter").arg(filters);
  const QString pin = m_doc->browse || pinKey.isEmpty() ? QString() : tr("%1 pin").arg(pinKey);
  auto list = [](QStringList entries) {
    entries.removeAll(QString());
    return entries.join(QStringLiteral(" · "));
  };
  const QString hints = m_tool.id == "area" ? (done ? (m_doc->browse ? list({click, back, filter}) : list({click, pin, back})) : picked ? list({click, back, filter}) : list({cancel, change}))
      : m_viewport->selectionFilter() == Viewport::SelFilter::Vertex && !done ? list({tr("%1-click arc to select center").arg(keys::fixedText("ctrl")), back})
      : done ? list({pin, clear, filter}) : picked ? list({back, change}) : list({cancel, change});
  m_prompt->set(m_tool.icon, m_tool.title, steps, hints);
  m_prompt->show();
  positionOverlays();
  if (!toolMeasures()) return;

  const Viewport::SelFilter f = m_viewport->selectionFilter();
  // As the filter is named now: a drawing's objects and points in 2D (UI-118).
  const QString kinds = action(f == Viewport::SelFilter::Face ? "select.faces" : f == Viewport::SelFilter::Edge ? "select.edges" : f == Viewport::SelFilter::Vertex ? "select.vertices" : "select.bodies")->text().remove('&');
  m_toolPanel->setContext(m_tool.steps ? tr("%1 · %2 of %3").arg(kinds.toLower()).arg(picked).arg(m_tool.steps) : tr("%1 · %2 picked").arg(kinds.toLower()).arg(picked));
  m_toolSteps->setSteps(steps, m_toolHover);
  // Points in a component's axes (UI-144): offered for what gives points (not a box, nor an area's outline).
  const std::string frameBody = m_toolPicks.empty() ? std::string() : m_toolPicks.front().body;
  const std::string component = m_tool.id == "bbox" || m_tool.id == "area" ? std::string() : measureComponent(frameBody);
  const QString frameName = component.empty() ? QString() : m_doc->nodeName(component);
  m_toolSteps->setFrameOptions(component.empty() ? QStringList() : QStringList{tr("World"), tr("%1 (component)").arg(frameName)}, m_measureFrame);
  gp_Trsf toWorld;
  const bool framed = !component.empty() && measureFrame(frameBody, toWorld);
  m_viewport->setMeasurementFrame(framed ? toWorld : gp_Trsf());
  // While picks are asked for, the steps above (and the guide's clip) say what to do: the summary under them is the
  // tool's one sentence when no guide plays, never the header and the waiting step again (UI-116). Done: the result; an
  // area not closed yet says why (UI-90).
  QString explanation;
  if (open) {
    const int ends = m_lastMeasure.value("open_ends", 0);
    explanation = m_lastMeasure.value("points", 0) ? tr("Pick at least three points: the area closes back to the first.")
                  : ends ? tr("Not closed yet: loose ends %1. Pick the objects that close it.").arg(ends)
                         : tr("These objects enclose nothing. Pick a closed object, or every object around the area.");
  } else if (!done) {
    const QString command = "inspect." + m_tool.id;
    const CommandHelp* h = help::find(command);
    explanation = (ToolGuide::enabled() && clips::has(command)) || !h ? QString() : h->summary;
  } else if (m_tool.id == "area") {
    explanation = m_lastMeasure.value("grown", false) ? tr("The smaller area beside the clicked part of the object, closed by the objects it meets or crosses. Pick more to give the boundary yourself.")
                  : m_lastMeasure.value("points", 0)  ? tr("The polygon through the picked points, closed back to the first.")
                  : m_lastMeasure.value("trimmed", false) ? tr("The area inside the picked objects, trimmed where they cross; areas inside it are holes.")
                                                          : tr("The area inside the picked boundary; areas inside it are holes.");
  } else {
    if (m_tool.id == "distance" && m_lastMeasure.contains("anchors")) explanation = tr("Click an anchor marker to move that measurement point. Edges stay selected until Esc or Clear. Choose a preset pair below.");
    else if (m_tool.id == "distance" && m_distanceMode == 1)
      explanation = framed ? tr("Distance between the centres of the selections: circle and sphere centres, cylinder axes, face and body centroids. Δ = point 2 − point 1 in the axes of %1.").arg(frameName)
                           : tr("Distance between the centres of the selections: circle and sphere centres, cylinder axes, face and body centroids. Δ = point 2 − point 1 in world axes.");
    else if (m_tool.id == "distance" && m_distanceMode == 2)
      explanation = framed ? tr("Largest distance between the selections, between their farthest points. Δ = point 2 − point 1 in the axes of %1.").arg(frameName)
                           : tr("Largest distance between the selections, between their farthest points. Δ = point 2 − point 1 in world axes.");
    else if (m_tool.id == "distance")
      explanation = framed ? tr("Shortest distance between the selections. Δ = point 2 − point 1 in the axes of %1.").arg(frameName)
                           : tr("Shortest distance between the selections. Δ = point 2 − point 1 in world axes.");
    else if (m_tool.id == "angle") explanation = tr("Directions compared at a common origin. Planar faces use their normals; curved faces use their axes.");
    else if (m_tool.id == "radius" && m_lastMeasure.contains("recognized"))
      explanation = tr("Radius from the center or cylinder axis to the surface. The surface is free-form (a B-spline) and is measured as the %1 it matches.").arg(i18n::t(QString::fromStdString(m_lastMeasure["recognized"].get<std::string>())));
    else if (m_tool.id == "radius") explanation = tr("Radius from the center or cylinder axis to the surface.");
    else if (m_tool.id == "length" && m_lastMeasure.value("kind", "") == "length") explanation = tr("Length of the edge along its curve, and the perimeter of each face loop it belongs to.");
    else if (m_tool.id == "length" && m_toolPicks.size() == 1 && m_toolPicks.front().kind == opad::Ref::Kind::Body) explanation = tr("Surface area of the body, and its volume when it is a solid.");
    else if (m_tool.id == "length") explanation = tr("Area of the face, its perimeter (every loop, seams left out) and its outer loop's.");
    else explanation = tr("Bounding box aligned with the world X, Y and Z axes.");
  }
  m_toolSteps->setSummary(done || open ? m_tool.title : QString(), explanation, open ? tr("open") : done && !m_doc->browse ? tr("unpinned") : QString());
  m_toolSteps->setError(m_toolError);
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
  m_toolSteps->setModeOptions(m_tool.id == "distance" ? QStringList{tr("Minimum"), tr("Centre to centre"), tr("Maximum")} : QStringList(), m_distanceMode);
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
    m_toolSteps->footer()->setCancel(tr("Back"));  // Esc takes the last pick back
    m_toolSteps->setFooter(done, !m_doc->browse && !measuredExploded());
    refreshMeasureHistory();
    return;
  }
  if (done) rows = measureRows(m_lastMeasure);
  // Where each pick was clicked (UI-144): while the next pick is awaited, and for a one-pick tool's result.
  const gp_Trsf toFrame = toWorld.Inverted();
  for (size_t i = 0; i < m_toolPoints.size() && i < m_toolPicks.size(); ++i)
    if (m_toolPoints[i].first && (!done || m_tool.steps == 1)) {
      gp_Pnt at(m_toolPoints[i].second[0], m_toolPoints[i].second[1], m_toolPoints[i].second[2]);
      if (framed) at.Transform(toFrame);
      if (framed && !done && rows.isEmpty()) rows << qMakePair(tr("Coordinates in"), frameName);
      rows << qMakePair(m_tool.steps == 1 ? tr("Picked at") : tr("Pick %1 at").arg(i + 1), units::vector(units::Kind::Length, opad::Vec3{at.X(), at.Y(), at.Z()}));
    }
  for(size_t i=0;i<m_toolPicks.size();++i) {
    const auto info=m_viewport->circleInfo(m_toolPicks[i]);
    if(info.contains("diameter")) rows << qMakePair(tr("Circle %1 diameter").arg(i+1),units::format(units::Kind::Length,info["diameter"].get<double>()));
    if(info.contains("segments")) rows << qMakePair(tr("Circle %1 mesh segments (approximate)").arg(i+1),QString::number(info["segments"].get<int>()));
  }
  m_toolSteps->setResult(rows);
  m_toolSteps->footer()->setCancel(tr("Clear"));
  m_toolSteps->setFooter(done, !m_doc->browse && !measuredExploded());
  refreshMeasureHistory();
}

// Results come in mm, mm² and degrees; they are shown in the document's unit and precision (UI-123).
QString MainWindow::measureTitle(const opad::json& r) const {
  const std::string kind = r.value("kind", "distance"), mode = r.value("mode", "");
  if (kind == "distance") return mode == "center" ? tr("Centre to centre") : mode == "max" ? tr("Maximum distance") : tr("Distance");
  if (kind == "angle") return tr("Angle");
  if (kind == "radius") return tr("Radius");
  if (kind == "length") return tr("Length");
  if (kind == "area") return tr("Area");
  return tr("Bounding box");
}

// UI-144: points can be given in the axes of the component the first pick lies in (its parent; a body at the root has
// none, nor one whose component lies as the world does: an imported file's root, mostly). A placement that is not rigid
// leaves them in world axes.
std::string MainWindow::measureComponent(const std::string& body) const {
  const opad::Node* n = body.empty() ? nullptr : m_doc->scene.node(body);
  return n && !n->parent.empty() && m_doc->scene.node(n->parent) && !m_doc->scene.world(n->parent).is_identity() ? n->parent : std::string();
}

bool MainWindow::measureFrame(const std::string& body, gp_Trsf& toWorld) const {
  const std::string component = m_measureFrame == 1 ? measureComponent(body) : std::string();
  if (component.empty()) return false;
  try {
    toWorld = opad::trsf_from_mat(m_doc->scene.world(component));
  } catch (const opad::Error&) {
    return false;
  }
  return true;
}

QList<QPair<QString, QString>> MainWindow::measureRows(const opad::json& r) const {
  QList<QPair<QString, QString>> rows;
  const std::string kind = r.value("kind", "distance"), unit = r.value("unit", "mm");
  const units::Kind L = units::Kind::Length;
  const opad::json refs = r.value("refs", opad::json::array());
  const std::string body = !refs.empty() && refs[0].is_string() ? opad::Ref::parse(refs[0].get<std::string>()).body : std::string();
  gp_Trsf toWorld;
  const bool framed = kind != "bbox" && measureFrame(body, toWorld);  // a box stays aligned with the world
  const gp_Trsf toFrame = toWorld.Inverted();
  auto vec = [L, framed, &toFrame](const opad::json& p) {
    gp_Pnt q(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
    if (framed) q.Transform(toFrame);
    return units::vector(L, opad::Vec3{q.X(), q.Y(), q.Z()});
  };
  if (r.contains("value")) rows << qMakePair(measureTitle(r), units::format(unit == "deg" ? units::Kind::Angle : unit == "mm2" ? units::Kind::Area : L, r["value"].get<double>()));
  if (framed) rows << qMakePair(tr("Coordinates in"), m_doc->nodeName(measureComponent(body)));
  if (r.contains("delta")) {
    gp_Vec delta(r["delta"][0].get<double>(), r["delta"][1].get<double>(), r["delta"][2].get<double>());
    if (framed) delta.Transform(toFrame);
    for (int i = 0; i < 3; ++i) {
      const double d = delta.Coord(i + 1);
      const bool plus = d > 0 && units::number(L, d) != units::number(L, 0);
      rows << qMakePair(tr("Δ%1").arg(QChar("XYZ"[i])), (plus ? "+" : "") + units::format(L, d));
    }
  }
  if (r.value("approximate", false) && r.contains("tolerance_mm")) rows << qMakePair(tr("Accuracy"), tr("within %1").arg(units::format(L, r["tolerance_mm"].get<double>())));
  if (r.contains("supplement")) rows << qMakePair(tr("Supplement"), units::format(units::Kind::Angle, r["supplement"].get<double>()));
  if (r.contains("diameter")) rows << qMakePair(tr("Diameter"), units::format(L, r["diameter"].get<double>()));
  if (r.contains("recognized"))  // a B-spline cylinder or circle (UI-50): what it was taken for, and how closely
    rows << qMakePair(tr("Recognised as"), tr("%1, within %2").arg(i18n::t(QString::fromStdString(r["recognized"].get<std::string>())), units::format(L, r.value("deviation", 0.0), 4)));
  for (const char* k : {"size", "min", "max"})
    if (r.contains(k) && r[k].is_array() && r[k].size() == 3) rows << qMakePair(i18n::t(QString("bbox %1").arg(k)), vec(r[k]));
  if (r.contains("relation") && r["relation"].is_string()) rows << qMakePair(tr("Relation"), i18n::t(QString::fromStdString(r["relation"].get<std::string>())));
  // Lengths and areas (UI-144): the loops an edge belongs to, a face's perimeters, a solid's volume.
  for (const auto& loop : r.value("loops", opad::json::array()))
    if (loop.is_object()) rows << qMakePair(loop.value("outer", true) ? tr("Loop on face %1").arg(loop.value("face", 0)) : tr("Inner loop on face %1").arg(loop.value("face", 0)), units::format(L, loop.value("perimeter", 0.0)));
  if (r.contains("perimeter")) rows << qMakePair(tr("Perimeter"), units::format(L, r["perimeter"].get<double>()));
  if (r.value("loops", opad::json()).is_number() && r["loops"].get<int>() > 1) {
    if (r.contains("outer_perimeter")) rows << qMakePair(tr("Outer loop"), units::format(L, r["outer_perimeter"].get<double>()));
    rows << qMakePair(tr("Loops"), QString::number(r["loops"].get<int>()));
  }
  if (r.value("holes", 0) > 0) rows << qMakePair(tr("Holes"), QString::number(r.value("holes", 0)));  // an Area tool's (UI-90)
  if (r.contains("volume")) rows << qMakePair(tr("Volume"), units::format(units::Kind::Volume, r["volume"].get<double>()));
  // The measured points, world XYZ (UI-144): where a distance starts and ends (the centres it took), a radius's centre.
  if (kind == "distance" && r.contains("point_a") && r.contains("point_b")) {
    const QString a = r.contains("centre_a") ? tr("Point 1 (%1)").arg(i18n::t(QString::fromStdString(r["centre_a"].get<std::string>()))) : tr("Point 1");
    const QString b = r.contains("centre_b") ? tr("Point 2 (%1)").arg(i18n::t(QString::fromStdString(r["centre_b"].get<std::string>()))) : tr("Point 2");
    rows << qMakePair(a, vec(r["point_a"])) << qMakePair(b, vec(r["point_b"]));
  } else if (kind == "radius" && r.contains("point_a")) rows << qMakePair(tr("Centre"), vec(r["point_a"]));
  else if (kind == "angle" && r.contains("vertex")) rows << qMakePair(tr("Vertex"), vec(r["vertex"]));
  else if (kind == "length" && r.contains("start") && !r.value("closed", false)) rows << qMakePair(tr("Start point"), vec(r["start"])) << qMakePair(tr("End point"), vec(r["end"]));
  else if (kind == "length" && r.contains("point")) rows << qMakePair(tr("Midpoint"), vec(r["point"]));
  else if (kind == "area" && r.contains("point")) rows << qMakePair(tr("Centroid"), vec(r["point"]));
  return rows;
}

void MainWindow::copyMeasurement(const opad::json& result) {
  QStringList lines;
  for (const auto& [key, value] : measureRows(result)) lines << key + "\t" + value;
  QApplication::clipboard()->setText(lines.join('\n'));
}

// The earlier results under the current one (the newest is the result shown, unless it was cleared).
void MainWindow::refreshMeasureHistory() {
  if (!toolMeasures()) return;
  QList<ToolHistoryRow> rows;
  for (size_t i = m_lastMeasure.is_null() ? 0 : 1; i < m_measureHistory.size(); ++i) {
    const opad::json& r = m_measureHistory[i].result;
    const auto values = measureRows(r);
    rows << ToolHistoryRow{measureTitle(r), values.isEmpty() ? QString() : values.front().second, m_measureHistory[i].pinned};
  }
  m_toolSteps->setHistory(rows, !m_doc->browse);
}

void MainWindow::pinMeasurement(opad::json result) {
  const bool current = result.is_null();
  if (current) result = m_lastMeasure;
  if (result.is_null()) return;
  if (result.value("exploded", false))
    return statusBar()->showMessage(tr("Pinned measurements are taken on the assembled model: collapse the exploded view, then measure again."), 6000);
  if (!requireEditable([this, result] { pinMeasurement(result); })) return;  // a pinned measurement is part of the document
  opad::json op;
  op["op"] = "measurement";
  op["kind"] = result.value("kind", "distance");
  opad::json refs = opad::json::array();
  for (const auto& r : result.value("refs", opad::json::array())) refs.push_back(opad::Ref::parse(r.get<std::string>()).to_json());
  op["refs"] = refs;
  op["result"] = result;
  opad::json r = m_doc->run("append", opad::json{{"op", op}});
  for (auto& record : m_measureHistory)
    if (record.result == result) record.pinned = true;
  if (!current) return refreshMeasureHistory();
  if (r.contains("appended") && !r["appended"].empty()) m_timeline->setCurrentOp(r["appended"][0].get<std::string>());
  m_toasts->toast(help::expand(tr("Measurement pinned. Manage it in Annotations ({key:panel.annotations}).")), tr("Show"), [this] { if (!action("panel.annotations")->isChecked()) action("panel.annotations")->trigger(); });
  if (!m_tool.id.isEmpty()) m_viewport->clearSelection();  // the tool stays on for the next measurement
}

bool MainWindow::measuredExploded() const { return m_lastMeasure.is_object() && m_lastMeasure.value("exploded", false); }

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
