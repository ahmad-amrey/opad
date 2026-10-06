#include "MainWindow.hpp"
#include "AgentBridge.hpp"
#include "CheckPanel.hpp"
#include "DrawingPlacer.hpp"
#include "KeyGuard.hpp"
#include "RecoveryManager.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFile>
#include <QInputDialog>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QStatusBar>
#include <QTimer>
#include <QStyle>
#include <QToolButton>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <memory>

#include "CommandHelp.hpp"
#include "I18n.hpp"
#include "KeyText.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "Units.hpp"

// The window's other parts live in MainWindow<Area>.cpp: File, View, Panels, Ribbon (menus, ribbon, Tools), Status (status
// bar, git), Selection (selection, properties, context menu), Inspect, Annotate, Edit, Design and Bench.

MainWindow::MainWindow() : m_doc(new AppDocument(this)) {
  m_doc->viewerOpens = m_settings.value("files/viewerMode", true).toBool();
  setWindowTitle("OPAD");
  setWindowIcon(icons::appIcon());
  resize(1600, 1000);
  setMinimumSize(1280, 800);
  setAcceptDrops(true);
  applyTheme(m_settings.value("ui/dark", true).toBool());
  shortcuts::migrate(m_settings);
  buildActions();
  createAreas();
  buildMenus();
  buildCentral();
  buildSnapCommands();
  buildRibbon();
  buildDocks();
  buildStatusBar();
  buildDesign();
  m_recovery=new RecoveryManager(m_doc,m_design,m_jobs,this);
  m_agent=new AgentBridge(m_doc,m_design,m_viewport,m_jobs,this);
  connect(m_agent,&AgentBridge::openRequested,this,[this](const QString& path){openPath(path);});  // open_document / new_document
  m_agent->bench();
  auto* agentStatus=new QToolButton(this);
  statusBar()->addPermanentWidget(agentStatus);
  auto updateAgentStatus=[this,agentStatus]{agentStatus->setText(tr("AI: %1").arg(m_agent->statusSummary()));agentStatus->setToolTip(m_doc->title());};
  connect(m_agent,&AgentBridge::statusChanged,agentStatus,updateAgentStatus);updateAgentStatus();
  // The selection is published only while agent access is on (UI-06): at once when it comes on, and the file goes with it.
  connect(m_agent,&AgentBridge::statusChanged,this,[this]{
    if(std::exchange(m_selPublishing,m_agent->publishesSelection())==m_selPublishing)return;
    if(m_selPublishing)scheduleSelectionSync();
    else unpublishSelection();
  });
  m_selPublishing=m_agent->publishesSelection();
  connect(agentStatus,&QToolButton::clicked,m_agent,&AgentBridge::settings);
  connect(m_recovery,&RecoveryManager::status,this,[this](const QString& text){statusBar()->showMessage(text,8000);});

  connect(m_doc, &AppDocument::aboutToReplace, this, [this] {
    if (m_loadJob && m_loadDocDone) m_loadJob->cancel();  // the document whose bodies stream in goes: so does its stream
    saveLastView();
    m_viewPath.clear();
    cancelPendingPick();
    cancelTool();
    clearMeasurement();
    m_measureHistory.clear();  // results of the document that goes
    m_viewport->clearPreviewBodies();
    clearCheckOverlays();  // a check's findings belong to the document that goes, and so does its panel
    if (m_toolStack->currentWidget() == m_checks && m_toolPanel->isVisible()) m_toolPanel->hide();
    m_viewport->clearCandidates();
    m_viewport->isolate({});
    if (action("inspect.section")->isChecked()) action("inspect.section")->setChecked(false);
  });
  connect(m_doc, &AppDocument::changed, this, [this] {
    trace::Scope scope("MainWindow: document changed");
    const bool replaced = std::exchange(m_areaGeneration, m_doc->generation) != m_doc->generation;
    // Nothing of the last document stays (UI-09): the viewer card, 2D mode (a viewed drawing's or one set by hand: a
    // view of that document; a drawing viewed next keeps it, loadFinished turns it on for one), what the status said was
    // under the pointer (the next frame says it again).
    if (replaced && action("view.2d")->isChecked() && !viewingDrawing()) setAutoTwoD(false);
    if (replaced && !m_doc->loading && !viewingDrawing()) followDrawing(false);  // Ctrl+N or a close: out of Drafting
    if (replaced && m_autoEdges && !m_loadJob) {  // Ctrl+N or a close (a file opened sets its own in openPath)
      m_autoEdges = false;
      m_viewport->setSelectionFilter(Viewport::SelFilter::Body);
    }
    m_viewport->clearHover();
    updateTitle();
    rebuildViewsMenu();
    updateViewerCard();
    updateChips();
    showDocument(m_doc->hasDocument);
    forEachArea([replaced](AreaController* area) { area->documentChanged(replaced); });
  });
  connect(m_doc, &AppDocument::loadFinished, this, [this](bool ok, const QString&) {
    if (!ok) return;
    if (const QString newer = newerRecords(); !newer.isEmpty()) m_toasts->toast(newer, QString(), {}, 10000);  // UI-65
    const auto bodies = m_doc->scene.all_bodies();
    const bool drawing = !bodies.empty() && std::all_of(bodies.begin(), bodies.end(), [this](const auto& id) { return m_doc->scene.node(id)->representation == "drawing2d"; });
    // Drawing files get a useful initial view. Viewing one (DXF, DWG, SVG) also turns 2D mode on; any other document
    // starts without it (the changed handler above).
    if (drawing) {
      m_viewport->standardView("top");
      if (m_viewport->selectionFilter() != Viewport::SelFilter::Edge) m_autoEdges = true;
      m_viewport->setSelectionFilter(Viewport::SelFilter::Edge);
    }
    const bool viewing = drawing && m_doc->browse;
    if (viewing && !action("view.2d")->isChecked()) setAutoTwoD(true);
    followDrawing(viewing);  // a drawing file viewed: Drafting (UI-104); anything else: back from it
  });
  connect(m_doc, &AppDocument::newDocumentCreated, m_viewport, [this] { m_viewport->home(); });
  // Viewer mode -> editable: the same shapes under content keys, so what is on screen stays (no second tessellation).
  connect(m_doc, &AppDocument::bodyKeysRenamed, m_viewport, &Viewport::renameBodyKeys);
  connect(m_chips, &ViewportChips::saveToEditRequested, this, [this] {  // a card left from a viewed file: gone, no silent no-op
    if (m_doc->browse || m_doc->readOnly) guarded([this] { m_doc->readOnly ? saveReadOnlyCopy() : saveViewerAs(); });
    else updateViewerCard();
  });
  connect(m_doc, &AppDocument::pathChanged, this, [this] { if(!m_doc->loading && !m_doc->browse) m_viewPath=m_doc->path(); updateTitle(); updateViewerCard(); });
  connect(m_doc, &AppDocument::message, this, [this](const QString& t) { statusBar()->showMessage(t, 6000); });
  connect(m_doc, &AppDocument::saved, this, [this] {
    const QFileInfo file(m_doc->path());
    resultToast(tr("Saved %1").arg(file.fileName()), file.absolutePath());
  });
  connect(m_viewport, &Viewport::selectionChanged, this, &MainWindow::onViewportSelection);
  connect(m_viewport, &Viewport::hoverChanged, m_statusHover, &QLabel::setText);
  connect(m_viewport, &Viewport::contextMenuRequested, this, [this](const QPoint& p) { showContextMenu(p, currentNodeIds()); });
  connect(m_viewport, &Viewport::isolationChanged, this, [this] {
    action("view.unisolate")->setEnabled(m_viewport->isIsolated());
    updateChips();
  });
  connect(m_viewport, &Viewport::meshingProgress, this, [this](int remaining) {
    m_meshRemaining = remaining;
    if (remaining > m_meshTotal) m_meshTotal = remaining;
    if (m_loadJob && m_loadDocDone && !m_loadJob->cancelled()) {
      if (remaining == 0) m_loadJob->finish();
      else setLoadPhase(meshPhase(), m_meshTotal > 0 ? (m_meshTotal - remaining) * 100 / m_meshTotal : 0);
    }
    if (m_loadJob) return;
    // Bodies shown after the load (showing a hidden assembly, leaving isolation) stream in the same way: the same
    // status-bar progress, so it is clear when they are all there. (The strip appears after 0.5 s only.)
    if (remaining > 0) {
      if (!m_displayJob) {
        m_displayJob = m_jobs->begin(tr("Displaying bodies"));
        m_displayTotal = 0;
        m_viewport->setStreamJob(m_displayJob);
        connect(m_displayJob, &Job::cancelRequested, m_viewport, &Viewport::cancelMeshing);
        connect(m_displayJob, &Job::finished, this, [this](bool, const QString&) { m_displayJob = nullptr; });
      }
      m_displayTotal = std::max(m_displayTotal, remaining);
      m_displayJob->setPhase(tr("Tessellating and displaying bodies (%1 of %2)").arg(m_displayTotal - remaining).arg(m_displayTotal),
                             (m_displayTotal - remaining) * 100 / m_displayTotal);
    } else if (m_displayJob) {
      m_displayJob->finish();
    }
  });
  // selection.json is written on a short debounce, only while agent access is on, and by a worker (UI-06).
  m_selFileTimer.setSingleShot(true);
  m_selFileTimer.setInterval(250);
  connect(&m_selFileTimer, &QTimer::timeout, this, &MainWindow::writeSelectionFile);
  connect(m_viewport, &Viewport::selectionApplied, this, &MainWindow::scheduleSelectionSync);
  // Loads run on a worker thread inside AppDocument; they report into the load job (see beginLoad).
  connect(m_doc, &AppDocument::loadProgress, this, &MainWindow::setLoadPhase);
  connect(m_doc, &AppDocument::loadFinished, this, [this](bool ok, const QString& err) {
    m_loadDocDone = true;
    if (!m_loadJob) return;  // already cancelled from the strip
    if (!ok) {
      m_loadJob->finish(false, err);
      return;
    }
    // Built (UI-40): the workspace is free at once, the bodies stream in under the load's progress and Cancel, and edits
    // wait for them (addCommand). The camera the file was last seen with comes now, not over a view being worked in.
    setLoading(false);
    if (m_afterLoad) m_afterLoad();
    m_afterLoad = nullptr;
    if (!m_loadDone.isEmpty()) m_loadJob->setDoneText(m_loadDone.arg(QLocale().toString(static_cast<qulonglong>(m_doc->scene.all_bodies().size()))));
    restoreLastView();
    if (m_meshRemaining > 0) setLoadPhase(meshPhase(), m_meshTotal > 0 ? (m_meshTotal - m_meshRemaining) * 100 / m_meshTotal : 0);
    else m_loadJob->finish();
  });
  trace::installUiWatchdog(this);  // logs any UI-thread stall over OPAD_TRACE_STALL_MS (OPAD_TRACE, UI-11)
  // A name typed into the browser is not taken as one-key commands (UI-09), nor one typed right after a dialog closed.
  m_keyGuard = new KeyGuard([browser = QPointer<BrowserPanel>(m_browser)] { return browser ? browser->renameEditor() : nullptr; }, this);
  qApp->installEventFilter(m_keyGuard);
  m_browserOverlay->setHold([browser = QPointer<BrowserPanel>(m_browser)] { return browser && browser->renameEditor(); });
  connect(m_browser, &BrowserPanel::selectionChanged, this, &MainWindow::onBrowserSelection);
  connect(m_browser, &BrowserPanel::contextMenuRequested, this, [this](const QPoint& p, const std::vector<std::string>& ids) { showContextMenu(p, ids); });
  connect(m_browser, &BrowserPanel::documentMenuRequested, this, [this](const QPoint& p) { showContextMenu(p, {}, true); });
  connect(m_browser, &BrowserPanel::fitRequested, m_viewport, [this](const std::vector<std::string>& ids) { m_viewport->fitNodes(ids, true); });
  connect(m_browser, &BrowserPanel::commandRequested, this, [this](const QString& id) { if (QAction* a = action(id); a && a->isEnabled()) a->trigger(); });
  connect(m_annotations, &AnnotationsPanel::addRequested, this, [this] { startAnnotation(false); });
  connect(m_annotations, &AnnotationsPanel::resolveRequested, this, &MainWindow::deleteOp);
  connect(m_annotations, &AnnotationsPanel::restoreRequested, this, &MainWindow::restoreOp);
  connect(m_annotations, &AnnotationsPanel::styleRequested, this, &MainWindow::restyleAnnotation);
  connect(m_annotations, &AnnotationsPanel::targetRequested, this, &MainWindow::showAnnotationCardTarget);
  m_noteCards = new NoteCards(m_doc, m_viewport, this);
  connect(m_annotations,&AnnotationsPanel::typeFilterChanged,m_noteCards,&NoteCards::setTypeFilter);
  connect(m_noteCards, &NoteCards::resolveRequested, this, &MainWindow::deleteOp);
  connect(m_noteCards, &NoteCards::styleRequested, this, &MainWindow::restyleAnnotation);
  connect(m_noteCards, &NoteCards::pressed, this, [this](const std::string& opId, const std::string& body) {
    m_timeline->setCurrentOp(opId);
    if (!body.empty()) { onBrowserSelection({body}); m_browser->setSelectedIds({body}); }
  });
  connect(m_props, &PropertiesPanel::faceChosen, this, [this](int index) {
    auto refs = m_viewport->selection();
    if (refs.empty()) return;
    opad::Ref r = refs.front();
    r.kind = opad::Ref::Kind::Face;
    r.index = index;
    showProperties({r});
  });
  connect(m_section, &SectionPanel::planeChanged, this, [this] {
    m_viewport->setSection(m_section->enabled(), m_section->origin(), m_section->normal(), m_section->caps());
    updateChips();
  });
  connect(m_viewport, &Viewport::sectionDragged, m_section, &SectionPanel::setOrigin);  // the plane's edge handle -> slider -> planeChanged
  connect(m_section, &SectionPanel::pickRequested, this, [this] { startTool("sectionface"); });
  connect(m_section, &SectionPanel::enabledChanged, this, [this](bool on) {
    if (action("inspect.section")->isChecked() != on) action("inspect.section")->setChecked(on);
  });
  // Its offset typed from the keyboard while its panel is open and no other tool runs (UI-122).
  m_section->takeValues(m_viewport, [this] {
    return m_sectionPanel->isVisible() && m_section->enabled() && !m_design->featureActive() && !m_design->sketchActive() && !m_design->pickingPlane() &&
           m_tool.id.isEmpty() && !m_drawingPlacer->active() && !m_annotationEditor;
  });
  // The check's clearance, overhang and wall, likewise while its panel is open.
  m_checks->takeValues(m_viewport, [this] {
    return m_toolPanel->isVisible() && m_toolStack->currentWidget() == m_checks && m_tool.id.isEmpty() && !m_design->featureActive() && !m_design->sketchActive() &&
           !m_design->pickingPlane() && !m_drawingPlacer->active() && !m_annotationEditor;
  });
  connect(m_section, &SectionPanel::saveRequested, this, [this](const QString& name, const opad::Vec3& o, const opad::Vec3& n) {
    if (!requireEditable()) return;
    bool ok = false;
    QString finalName = QInputDialog::getText(this, tr("Named section"), tr("Name:"), QLineEdit::Normal, name, &ok);
    if (!ok || finalName.isEmpty()) return;
    guarded([&] { m_doc->run("section", opad::json{{"name", finalName.toStdString()}, {"origin", o}, {"normal", n}}); });
  });
  connect(m_timeline, &TimelineWidget::opClicked, this, [this](const std::string& id) {
    if (!areaCommand("timeline.select", id)) selectOpTargets(id);  // a feature that changed bodies: the faces it made (SmartSelect)
  });
  connect(m_timeline, &TimelineWidget::opClicked, this, &MainWindow::resumePendingPick);  // a marker is what Edit feature waits for
  connect(m_timeline, &TimelineWidget::contextRequested, this, [this](const std::string& id,const QPoint& point) { guarded([&] { timelineMenu(id,point); }); });
  connect(m_toolSteps, &ToolStepsPanel::pinRequested, this, [this] { guarded([this] { pinMeasurement(); }); });
  connect(m_toolSteps, &ToolStepsPanel::clearRequested, this, &MainWindow::toolEscape);
  connect(m_toolSteps, &ToolStepsPanel::componentsChanged, m_viewport, &Viewport::setMeasurementComponents);
  connect(m_toolSteps, &ToolStepsPanel::modeChanged, this, [this](int mode) {  // Distance: minimum, centre to centre, maximum (UI-144)
    if (mode == m_distanceMode) return;
    m_distanceMode = mode;
    m_settings.setValue("measure/distanceMode", mode);
    if (m_tool.id == "distance") toolPicksChanged(m_viewport->selection(), false);  // measured again in the new mode
  });
  connect(m_toolSteps, &ToolStepsPanel::frameChanged, this, [this](int frame) {  // world or the first pick's component axes (UI-144)
    if (frame < 0 || frame == m_measureFrame) return;
    m_measureFrame = frame;
    m_settings.setValue("measure/frame", frame);
    refreshToolUi();
  });
  connect(m_toolSteps, &ToolStepsPanel::historyCopyRequested, this, [this](int row) {
    const size_t i = row + (m_lastMeasure.is_null() ? 0 : 1);
    if (i < m_measureHistory.size()) copyMeasurement(m_measureHistory[i].result);
  });
  connect(m_toolSteps, &ToolStepsPanel::historyPinRequested, this, [this](int row) {
    const size_t i = row + (m_lastMeasure.is_null() ? 0 : 1);
    if (i < m_measureHistory.size()) guarded([this, i] { pinMeasurement(m_measureHistory[i].result); });
  });
  connect(m_toolSteps, &ToolStepsPanel::anchorChanged, this, [this](int index) {
    if (!m_lastMeasure.contains("anchors") || index < 0 || index >= static_cast<int>(m_lastMeasure["anchors"].size())) return;
    const opad::json& anchor = m_lastMeasure["anchors"][index];
    for (const char* key : {"value", "point_a", "point_b", "delta"}) m_lastMeasure[key] = anchor[key];
    m_lastMeasure["anchor_index"] = index;
    m_viewport->showMeasurement(m_lastMeasure);
    refreshToolUi();
  });
  connect(m_viewport, &Viewport::measurementAnchorPicked, this, [this](int side, const opad::Vec3& point) {
    if (m_tool.id != "distance" || !m_lastMeasure.contains("anchors") || side < 0 || side > 1) return;
    m_lastMeasure[side == 0 ? "point_a" : "point_b"] = point;
    double squared = 0;
    for (int axis = 0; axis < 3; ++axis) {
      const double delta = m_lastMeasure["point_b"][axis].get<double>() - m_lastMeasure["point_a"][axis].get<double>();
      m_lastMeasure["delta"][axis] = delta;
      squared += delta * delta;
    }
    m_lastMeasure["value"] = std::sqrt(squared);
    auto& options = m_lastMeasure["anchors"];
    int index = -1;
    for (size_t i = 0; i < options.size(); ++i)
      if (options[i].value("kind", "") == "custom") { index = static_cast<int>(i); break; }
    if (index < 0) { index = static_cast<int>(options.size()); options.push_back({{"kind", "custom"}}); }
    for (const char* key : {"value", "point_a", "point_b", "delta"}) options[index][key] = m_lastMeasure[key];
    m_lastMeasure["anchor_index"] = index;
    m_viewport->showMeasurement(m_lastMeasure);
    refreshToolUi();
  });
  m_toolPanel->setEscapeHandler([this] { toolEscape(); });
  connect(m_toolPanel, &ToolPanel::visibilityChanged, this, [this](bool on) {
    // Closing the tool's panel (or another panel opening) leaves the tool or the check, and the check's colours go. Minimising
    // the window hides it too, spontaneously: isVisible() stays true then, and everything stays as it was.
    if (on || m_toolPanel->isVisible()) return;
    if (toolMeasures()) cancelTool();
    if (m_toolStack->currentWidget() == m_checks) endCheck();
  });
  connect(m_viewport, &Viewport::hoverChanged, this, [this](const QString& text) {
    if (m_tool.id.isEmpty() || text == m_toolHover) return;
    m_toolHover = text;
    if (toolMeasures() && static_cast<int>(m_toolPicks.size()) < m_tool.steps) m_toolSteps->setSteps(toolSteps(), m_toolHover);
  });
  // Distance, one end picked: a dashed line to where the mouse meets the hovered candidate, point to point ("≈").
  connect(m_viewport, &Viewport::hoverPoint, this, [this](bool valid, const opad::Vec3& p) {
    if (m_tool.id != "distance" || m_toolPicks.size() != 1 || m_toolPoints.empty() || !m_toolPoints[0].first) return;
    if (!valid) return m_viewport->clearPreview();
    const opad::Vec3& a = m_toolPoints[0].second;
    const double d = std::sqrt((a[0] - p[0]) * (a[0] - p[0]) + (a[1] - p[1]) * (a[1] - p[1]) + (a[2] - p[2]) * (a[2] - p[2]));
    m_viewport->showPreview(a, p, QString::fromUtf8("≈ %1").arg(units::format(units::Kind::Length, d)));
  });
  connect(m_empty, &EmptyState::openRequested, action("file.open"), &QAction::trigger);
  connect(m_empty, &EmptyState::newRequested, action("file.new"), &QAction::trigger);
  connect(m_empty, &EmptyState::recentChosen, this, [this](const QString& path) { openPath(path); });
  connect(m_empty, &EmptyState::filesDropped, this, [this](const QStringList& paths) { openPath(paths.first()); });
  connect(m_empty, &EmptyState::recentChanged, this, [this](const QStringList& paths) {  // removed or located on the start page
    m_settings.setValue("ui/recent", paths);
    rebuildRecentMenu();
  });
  connect(m_empty, &EmptyState::templateChosen, this, [this](const QString& id) { guarded([&] { newFromTemplate(id); }); });
  m_empty->setCommands([this](const QString& id) { return action(id); });
  m_empty->setJobs(m_jobs);
  m_empty->setMenuBuilder([this](const QString& path, QWidget* parent) { return recentMenu(path, parent); });  // File > Recent's

  restoreGeometry(m_settings.value("ui/geometry").toByteArray());
  if (m_settings.value("ui/layoutVersion").toInt() == 3) restoreState(m_settings.value("ui/state").toByteArray());
  // restoreState carries the corner layout of older sessions; the design fixes it, so re-apply.
  setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
  setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
  setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
  setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
  action("view.grid")->setChecked(m_settings.value("view/grid",false).toBool());
  action("view.ortho")->setChecked(m_settings.value("view/orthographic",true).toBool());
  const auto style=m_settings.value("view/style","view.edges").toString();
  if(style=="view.shaded" || style=="view.edges" || style=="view.wire" || style=="view.hidden" || style=="view.hiddenEdges") action(style)->trigger();
  m_empty->setRecent(recent());
  showDocument(false);
  updateTitle();
  updateChips();
  m_areaGeneration = m_doc->generation;
  shortcuts::settleAlternates(m_actions);  // every command is made: an alternate key another one uses goes
  keys::announce();  // and every key is final: what was labelled while the areas were still adding theirs reads them again
  for (AreaController* area : m_areas) area->ready();
  m_areasReady = true;
  if (!m_areas.empty()) positionOverlays();
}

// ---------------------------------------------------------------- actions
MainWindow::~MainWindow() {
  for (AreaController* area : std::exchange(m_areas, {})) delete area;  // first, while everything they use is there
  delete m_agent;m_agent=nullptr; // stop bridge jobs while the document and viewport still exist
  // QProcess can emit finished while QObject deletes children, after our status
  // widgets and C++ members are gone. Disconnect callbacks before base teardown.
  for(auto* child:findChildren<QObject*>())QObject::disconnect(child,nullptr,this,nullptr);
}
// A command whose record is made from the id (Commands.hpp): its group and scope by the id's area, editsDocument by
// isEditAction.
QAction* MainWindow::addAction(const QString& id, const QString& text, const QString& icon, const QKeySequence& shortcut, std::function<void()> fn, bool checkable) {
  CommandInfo info;
  info.id = id;
  info.label = text;
  info.icon = icon;
  info.key = shortcut;
  info.checkable = checkable;
  info.editsDocument = isEditAction(id);
  return addCommand(info, std::move(fn));
}

// The one factory of the window's commands: the QAction is made from the record and both go into the registry.
QAction* MainWindow::addCommand(const CommandInfo& info, std::function<void()> fn) {
  const QString id = info.id;
  auto* a = new QAction(info.label, this);
  a->setObjectName(id);
  a->setData(info.icon);
  if (!info.icon.isEmpty()) a->setIcon(icons::themed(info.icon));
  a->setProperty("fixedShortcut", info.fixedKey);
  shortcuts::initialize(a,info.key,m_settings);
  a->setCheckable(info.checkable);
  a->setShortcutContext(Qt::WindowShortcut);
  QString tip = info.label;
  tip.remove('&');
  if (!a->shortcut().isEmpty()) tip += "  (" + a->shortcut().toString(QKeySequence::NativeText) + ")";
  a->setToolTip(tip);
  connect(a, &QAction::triggered, this, [this, fn, id, a] {
    if (m_loadJob && !id.startsWith("file.") && !id.startsWith("panel.") && id != "view.dark") {
      if (!m_loadDocDone) return;  // reading and building: the workspace is locked
      if (m_commands.editsDocument(id)) return deferEdit(a);  // its bodies still stream in
    }
    m_viewport->resetHoverFade();
    // Another command drops one waiting for its selection; looking around (view, filters, panels, help) does not.
    static const QStringList looking{"view.", "select.", "nav.", "panel.", "help.", "workspace.", "edit.selectparent", "edit.filter", "edit.selectall", "edit.invert", "tools.commands"};
    if (!m_pendingPick.isEmpty() && std::none_of(looking.begin(), looking.end(), [&id](const QString& p) { return id.startsWith(p); })) cancelPendingPick();
    // A copy of the document is being taken (smart selection's, an agent's) or it is being saved: a change or another
    // document waits for that instead of failing.
    if (m_doc->snapshotBusy() && (m_commands.editsDocument(id) || id.startsWith("file.") || id.startsWith("design.") || id == "edit.hide" || id == "edit.showall")) {
      if (a->isCheckable()) { QSignalBlocker block(a); a->setChecked(!a->isChecked()); }
      return m_doc->afterCapture([a] { a->trigger(); });
    }
    if (m_doc->viewOnly() && m_commands.editsDocument(id)) {  // viewer mode, read-only: offered, and asks to save first
      if (a->isCheckable()) { QSignalBlocker block(a); a->setChecked(!a->isChecked()); }
      requireEditable([a] { a->trigger(); });
      return;
    }
    // Rolled back with the timeline's marker (UI-99): a change goes at the end, so the model is rolled forward first,
    // and picks on the bodies as they were with it (TimelineArea).
    if (m_doc->rolledBack() && m_commands.editsDocument(id)) {
      // Picked faces and edges are the earlier state's and go as it rolls forward: Del on them must not fall through to
      // their body or to the timeline's marker (UI-04).
      if (id == "edit.delete" && !m_timeline->hasFocus() && std::any_of(m_selRefs.begin(), m_selRefs.end(), [](const opad::Ref& r) { return r.kind != opad::Ref::Kind::Body; }))
        return statusBar()->showMessage(tr("Rolled back: these faces and edges are an earlier state's. Roll forward and pick them again to delete what made them."), 8000);
      if (QAction* forward = action("timeline.rollForward")) {
        forward->trigger();
        statusBar()->showMessage(tr("Rolled forward to the end of the timeline: the change is added there."), 6000);
      }
    }
    if (switchesToDesign(id)) {  // E in Review: its tools and panel are Design's (UI-104), with what was picked for it
      QScopedValueRollback<bool> command(m_commandSwitch, true);
      setWorkspace("design");
    }
    QScopedValueRollback<QString> running(m_runningCommand, id);
    guarded(fn);
    noteCommand(id);
  });
  m_commands.add(info, a);
  m_actions << a;
  QMainWindow::addAction(a);
  return a;
}

QAction* MainWindow::action(const QString& id) const { return m_commands.action(id); }

CommandContext MainWindow::commandContext() const {
  CommandContext c;
  c.document = m_doc->hasDocument;
  c.viewer = m_doc->browse;
  c.sketching = m_design && m_design->sketchActive();
  c.workspace = m_workspaceId;
  c.selection = selectionContext();
  return c;
}

void MainWindow::updateCommands() { m_commands.updateEnabled(commandContext()); }

void MainWindow::guarded(const std::function<void()>& fn) {
  try {
    fn();
  } catch (const opad::UserHint& h) {
    hint(QString::fromUtf8(h.what()), h.pick);
  } catch (const std::exception& e) {
    QMessageBox::warning(this, tr("OPAD"), help::expand(i18n::message(QString::fromUtf8(e.what()))));  // keys named by token
  }
}

// "Select the objects to colour first." goes by itself. With pick, the command that raised it waits: the next selection
// in the view or the browser, or a marker clicked on the timeline, runs it again; its toast stays until then, and its
// Cancel, Esc or another command drop the wait. Without a view to show it over (start page), the status bar says it.
void MainWindow::hint(const QString& text, bool pick) {
  cancelPendingPick();
  const QString shown = help::expand(i18n::t(text));  // a key named by token ({key:id}): the user's now
  if (!m_toasts || !m_viewport->isVisible()) return statusBar()->showMessage(shown, 6000);
  if (!pick || m_runningCommand.isEmpty()) {
    m_toasts->toast(shown, QString(), {}, 6000);
    return;
  }
  m_pendingPick = m_runningCommand;
  m_pendingToast = m_toasts->toast(shown, tr("Cancel"), [this] { m_pendingPick.clear(); }, 0);
  m_pendingToast->setProperty("pendingCommand", m_pendingPick);
  if (trace::enabled()) trace::log("hint: " + m_pendingPick + " waits for a selection");
}

void MainWindow::cancelPendingPick() {
  m_pendingPick.clear();
  if (m_pendingToast) m_pendingToast->dismiss();
}

void MainWindow::resumePendingPick() {
  if (m_pendingPick.isEmpty()) return;
  const QString id = std::exchange(m_pendingPick, QString());
  if (m_pendingToast) m_pendingToast->dismiss();
  if (trace::enabled()) trace::log("hint: " + id + " runs with the selection");
  QTimer::singleShot(0, this, [this, id] { if (QAction* a = action(id); a && a->isEnabled()) a->trigger(); });  // after the selection has settled
}

// A change that failed and left the document as it was (UI-109): a toast with a red edge for 10 s instead of a message box
// that stopped the work. Without a view to show it over, the box.
void MainWindow::failedToast(const QString& text) {
  if (!m_toasts || !m_viewport->isVisible()) return (void)QMessageBox::warning(this, tr("OPAD"), text);
  Toast* toast = m_toasts->toast(text, QString(), {}, 10000);
  toast->setProperty("kind", "error");
  toast->style()->unpolish(toast);
  toast->style()->polish(toast);
}

void MainWindow::resultToast(const QString& text, const QString& folder) {
  if (!m_toasts || !m_viewport->isVisible()) return statusBar()->showMessage(text, 6000);
  if (folder.isEmpty()) m_toasts->toast(text);
  else m_toasts->toast(text, tr("Open folder"), [folder] { QDesktopServices::openUrl(QUrl::fromLocalFile(folder)); });
}

// One builder per area, in this order: the order of m_actions is the command order (search palette).
void MainWindow::buildActions() {
  buildFileActions();
  buildViewActions();
  buildDesignActions();
  buildNavigationActions();
  buildInspectActions();
  buildAnnotateActions();
  buildEditActions();
  buildToolsActions();
}

void MainWindow::showDocument(bool has) {
  showCentral();
  m_browserOverlay->setVisible(has && action("panel.browser")->isChecked());
  for (QAction* a : m_actions) {
    QString id = a->objectName();
    const bool setting = id == "view.dark" || id == "view.cubeEdgesCorners";  // in the Settings menu: also without a document
    if (id.startsWith("view.") && !setting)  // no corner view in 2D mode (it keeps a principal plane)
      a->setEnabled(has && (id != "view.unisolate" || m_viewport->isIsolated()) && (id != "view.iso" || !action("view.2d")->isChecked()));
    if (id.startsWith("inspect.") || id.startsWith("annotate.") || id.startsWith("select.") || id == "file.export" || id == "file.screenshot" || id == "file.save" || id == "file.saveas" || id == "file.close")
      a->setEnabled(has);
    if (id == "file.importdoc") a->setEnabled(m_doc->browse);
    // Viewer mode keeps the editing commands: they say that the file has to be saved first (isEditAction).
  }
  if (m_pinAction) m_pinAction->setEnabled(has && !m_lastMeasure.is_null() && !measuredExploded());
  if (m_design) updateDesignState();
  m_browser->setViewerMode(m_doc->viewOnly());
  updateUndoActions();
  action("panel.annotations")->setEnabled(has && !m_doc->browse);
  action("panel.section")->setEnabled(has);
  if (!has) m_selRefs.clear();
  if (!has) cancelTool();
  for (ToolPanel* p : m_panels)
    if (!has || (p == m_annotationsPanel && m_doc->browse)) p->hide();
  if (m_doc->browse && m_timelineDock->isVisible()) { m_timelineDock->hide(); m_timelineHiddenByViewer = true; }
  else if (!m_doc->browse && m_timelineHiddenByViewer) { m_timelineDock->show(); m_timelineHiddenByViewer = false; }
  updateCommands();
}

bool MainWindow::maybeSave(std::function<void()> resume) {
  if (m_areasReady)
    for (AreaController* area : m_areas)
      if (!area->maybeClose()) return false;  // unfinished work in an area that the user did not give up
  if (m_benchSelect) return true;  // benches run in hidden windows: a question here would pop up on the user's desktop
  if(m_doc->snapshotBusy()){statusBar()->showMessage(tr("A snapshot is being captured. Try again shortly."),4000);return false;}
  if (!leaveSketch(std::move(resume))) return false;
  if (!m_doc->isDirty()) return true;
  std::unique_ptr<QMessageBox> box(unsavedPrompt());
  box->exec();
  QAbstractButton* clicked = box->clickedButton();
  if (!clicked || clicked->objectName() == "reviewChanges") return false;  // Compare opens; the question waits for later
  const auto r = box->standardButton(clicked);
  if (r == QMessageBox::Cancel) return false;
  if (r == QMessageBox::Save) {
    try {
      if (m_doc->doc.path.empty()) action("file.saveas")->trigger();
      else m_doc->save();
    } catch (const std::exception& e) {
      QMessageBox::warning(this, tr("OPAD"), i18n::t(QString::fromUtf8(e.what())));
      return false;
    }
    return !m_doc->isDirty();
  }
  return true;
}

QMessageBox* MainWindow::unsavedPrompt() {
  auto* box = new QMessageBox(QMessageBox::Question, tr("Unsaved changes"), tr("Save changes to %1?").arg(m_doc->path().isEmpty() ? tr("the document") : m_doc->path()),
                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
  box->setDefaultButton(QMessageBox::Save);
  if (QAction* review = action("vcs.unsavedChanges"); review && review->isEnabled()) {
    QPushButton* button = box->addButton(tr("Review changes…"), QMessageBox::ActionRole);
    button->setObjectName("reviewChanges");
    button->setToolTip(tr("Compare the saved file with this session before deciding"));
    connect(button, &QPushButton::clicked, review, [review] { QTimer::singleShot(0, review, &QAction::trigger); });  // once the question has closed
  }
  return box;
}

bool MainWindow::leaveSketch(std::function<void()> resume) {
  if (!m_design->sketchActive() || !m_design->sketch()->modified()) return true;
  const auto result = QMessageBox::question(this, tr("Unfinished sketch"), tr("Finish the sketch before continuing?"), QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
  if (result == QMessageBox::Discard) {
    m_design->sketch()->end();
    m_doc->setRollback({});
    return true;
  }
  if (result != QMessageBox::Save) return false;  // Cancel, or the box closed some other way
  QPointer<MainWindow> self(this);
  m_design->finishSketch([self, resume] { if (self && resume) QTimer::singleShot(0, self, resume); });  // after the sketch op is in
  return false;
}

void MainWindow::closeEvent(QCloseEvent* e) {
  if(m_closePending){e->ignore();return;}
  if (!m_recoveryClosed && !maybeSave([this] { close(); })) {
    e->ignore();
    return;
  }
  if(!m_recoveryClosed){
    e->ignore();m_closePending=true;setEnabled(false);
    QPointer<MainWindow> self(this);
    m_recovery->finishSession([self]{if(self){self->m_closePending=false;self->m_recoveryClosed=true;self->close();}});
    return;
  }
  saveLastView();
  m_settings.setValue("ui/geometry", saveGeometry());
  if (m_timelineHiddenByViewer) m_timelineDock->show();  // viewer mode hid it; do not save that as the user's layout
  m_settings.setValue("ui/state", saveState());
  m_settings.setValue("ui/layoutVersion", 3);
  e->accept();
}
