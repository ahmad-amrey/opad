#include "MainWindow.hpp"
#include "AgentBridge.hpp"
#include "CheckPanel.hpp"
#include "RecoveryManager.hpp"

#include <QCloseEvent>
#include <QInputDialog>
#include <QMessageBox>
#include <QPointer>
#include <QStatusBar>
#include <QToolButton>

#include <algorithm>
#include <cmath>

#include "I18n.hpp"
#include "Icons.hpp"
#include "Theme.hpp"

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
  buildRibbon();
  buildDocks();
  buildStatusBar();
  buildDesign();
  m_recovery=new RecoveryManager(m_doc,m_design,m_jobs,this);
  m_agent=new AgentBridge(m_doc,m_design,m_viewport,m_jobs,this);
  m_agent->bench();
  auto* agentStatus=new QToolButton(this);
  statusBar()->addPermanentWidget(agentStatus);
  auto updateAgentStatus=[this,agentStatus]{agentStatus->setText(tr("AI: %1").arg(m_agent->statusSummary()));agentStatus->setToolTip(m_doc->title());};
  connect(m_agent,&AgentBridge::statusChanged,agentStatus,updateAgentStatus);updateAgentStatus();
  connect(agentStatus,&QToolButton::clicked,m_agent,&AgentBridge::settings);
  connect(m_recovery,&RecoveryManager::status,this,[this](const QString& text){statusBar()->showMessage(text,8000);});

  connect(m_doc, &AppDocument::aboutToReplace, this, [this] {
    saveLastView();
    m_viewPath.clear();
    cancelTool();
    clearMeasurement();
    m_viewport->clearPreviewBodies();
    m_viewport->clearCandidates();
    m_viewport->isolate({});
    if (action("inspect.section")->isChecked()) action("inspect.section")->setChecked(false);
  });
  connect(m_doc, &AppDocument::changed, this, [this] {
    trace::Scope scope("MainWindow: document changed");
    updateTitle();
    rebuildViewsMenu();
    updateChips();
    showDocument(m_doc->hasDocument);
    const bool replaced = std::exchange(m_areaGeneration, m_doc->generation) != m_doc->generation;
    forEachArea([replaced](AreaController* area) { area->documentChanged(replaced); });
  });
  connect(m_doc, &AppDocument::loadFinished, this, [this](bool ok, const QString&) {
    if (!ok) return;
    const auto bodies = m_doc->scene.all_bodies();
    const bool drawing = !bodies.empty() && std::all_of(bodies.begin(), bodies.end(), [this](const auto& id) { return m_doc->scene.node(id)->representation == "drawing2d"; });
    // Drawing files get a useful initial view. Viewing one (DXF, DWG, SVG) also turns 2D mode on; the next file that
    // is not a drawing turns it off again, unless the toggle was changed by hand meanwhile.
    if (drawing) { m_viewport->standardView("top"); m_viewport->setSelectionFilter(Viewport::SelFilter::Edge); }
    QAction* flat = action("view.2d");
    const bool viewingDrawing = drawing && m_doc->browse;
    if (viewingDrawing != flat->isChecked() && (viewingDrawing || m_autoTwoD)) {
      m_settingTwoD = true;
      flat->setChecked(viewingDrawing);
      m_settingTwoD = false;
      m_autoTwoD = viewingDrawing;
    }
  });
  connect(m_doc, &AppDocument::newDocumentCreated, m_viewport, &Viewport::home);
  // Viewer mode -> editable: the same shapes under content keys, so what is on screen stays (no second tessellation).
  connect(m_doc, &AppDocument::bodyKeysRenamed, m_viewport, &Viewport::renameBodyKeys);
  connect(m_chips, &ViewportChips::saveToEditRequested, this, [this] { guarded([this] { saveViewerAs(); }); });
  connect(m_doc, &AppDocument::pathChanged, this, [this] { if(!m_doc->loading && !m_doc->browse) m_viewPath=m_doc->path(); updateTitle(); refreshGit(); });
  connect(m_doc, &AppDocument::message, this, [this](const QString& t) { statusBar()->showMessage(t, 6000); });
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
    if (m_loadJob && m_loadDocDone) {
      if (remaining == 0) m_loadJob->finish();
      else setLoadPhase(meshPhase(), m_meshTotal > 0 ? (m_meshTotal - remaining) * 100 / m_meshTotal : -1);
    }
    if (m_loadJob) return;
    // Bodies shown after the load (showing a hidden assembly, leaving isolation) stream in the same way: the same
    // status-bar progress, so it is clear when they are all there. (The strip appears after 0.5 s only.)
    if (remaining > 0) {
      if (!m_displayJob) {
        m_displayJob = m_jobs->begin(tr("Displaying bodies"));
        m_displayTotal = 0;
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
  // selection.json is written on a short debounce and off the hot selection path (it inspects geometry).
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
    if (m_afterLoad) m_afterLoad();
    m_afterLoad = nullptr;
    if (m_meshRemaining > 0) setLoadPhase(meshPhase(), m_meshTotal > 0 ? (m_meshTotal - m_meshRemaining) * 100 / m_meshTotal : -1);
    else m_loadJob->finish();
  });
  trace::installUiWatchdog(this);  // logs any UI-thread stall over 250 ms (OPAD_TRACE)
  connect(m_browser, &BrowserPanel::selectionChanged, this, &MainWindow::onBrowserSelection);
  connect(m_browser, &BrowserPanel::contextMenuRequested, this, [this](const QPoint& p, const std::vector<std::string>& ids) { showContextMenu(p, ids); });
  connect(m_browser, &BrowserPanel::fitRequested, m_viewport, &Viewport::fitNodes);
  connect(m_annotations, &AnnotationsPanel::addRequested, this, [this] { startAnnotation(false); });
  connect(m_annotations, &AnnotationsPanel::resolveRequested, this, &MainWindow::deleteOp);
  connect(m_annotations, &AnnotationsPanel::restoreRequested, this, &MainWindow::restoreOp);
  connect(m_annotations, &AnnotationsPanel::styleRequested, this, &MainWindow::restyleAnnotation);
  connect(m_annotations, &AnnotationsPanel::selectNode, this, [this](const std::string& id) { onBrowserSelection({id}); m_browser->setSelectedIds({id}); });
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
  connect(m_section, &SectionPanel::saveRequested, this, [this](const QString& name, const opad::Vec3& o, const opad::Vec3& n) {
    if (!requireEditable()) return;
    bool ok = false;
    QString finalName = QInputDialog::getText(this, tr("Named section"), tr("Name:"), QLineEdit::Normal, name, &ok);
    if (!ok || finalName.isEmpty()) return;
    guarded([&] { m_doc->run("section", opad::json{{"name", finalName.toStdString()}, {"origin", o}, {"normal", n}}); });
  });
  connect(m_timeline, &TimelineWidget::opClicked, this, &MainWindow::selectOpTargets);
  connect(m_timeline, &TimelineWidget::contextRequested, this, [this](const std::string& id,const QPoint& point) { guarded([&] { timelineMenu(id,point); }); });
  connect(m_toolSteps, &ToolStepsPanel::pinRequested, this, [this] { guarded([this] { pinMeasurement(); }); });
  connect(m_toolSteps, &ToolStepsPanel::clearRequested, this, &MainWindow::toolEscape);
  connect(m_toolSteps, &ToolStepsPanel::componentsChanged, m_viewport, &Viewport::setMeasurementComponents);
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
    if (!on && toolMeasures()) cancelTool();  // closing the tool's panel leaves the tool
    if (!on && m_toolStack->currentWidget() == m_checks) endCheck();
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
    m_viewport->showPreview(a, p, QString::fromUtf8("≈ %1 mm").arg(d, 0, 'f', 1));
  });
  connect(m_empty, &EmptyState::openRequested, action("file.open"), &QAction::trigger);
  connect(m_empty, &EmptyState::importRequested, action("file.new"), &QAction::trigger);
  connect(m_empty, &EmptyState::recentChosen, this, &MainWindow::openPath);
  connect(m_empty, &EmptyState::filesDropped, this, [this](const QStringList& paths) { openPath(paths.first()); });

  m_gitTimer.setInterval(5000);
  connect(&m_gitTimer, &QTimer::timeout, this, &MainWindow::refreshGit);
  m_gitTimer.start();

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
  if(style=="view.shaded" || style=="view.edges" || style=="view.wire") action(style)->trigger();
  m_empty->setRecent(recent());
  showDocument(false);
  updateTitle();
  updateChips();
  m_areaGeneration = m_doc->generation;
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
  shortcuts::initialize(a,info.key,m_settings);
  a->setCheckable(info.checkable);
  a->setShortcutContext(Qt::WindowShortcut);
  QString tip = info.label;
  tip.remove('&');
  if (!a->shortcut().isEmpty()) tip += "  (" + a->shortcut().toString(QKeySequence::NativeText) + ")";
  a->setToolTip(tip);
  connect(a, &QAction::triggered, this, [this, fn, id, a] {
    if (m_loadJob && !id.startsWith("file.") && !id.startsWith("panel.") && id != "view.dark") return;  // loading: workspace is locked
    m_viewport->resetHoverFade();
    if (m_doc->browse && m_commands.editsDocument(id)) {  // viewer mode: offered, and asks to save first
      if (a->isCheckable()) { QSignalBlocker block(a); a->setChecked(!a->isChecked()); }
      requireEditable([a] { a->trigger(); });
      return;
    }
    guarded(fn);
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
  } catch (const std::exception& e) {
    QMessageBox::warning(this, tr("OPAD"), i18n::t(QString::fromUtf8(e.what())));
  }
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
  m_stack->setCurrentIndex(has ? 1 : 0);
  m_browserOverlay->setVisible(has && action("panel.browser")->isChecked());
  for (QAction* a : m_actions) {
    QString id = a->objectName();
    if (id.startsWith("view.") && id != "view.dark") a->setEnabled(has && (id != "view.unisolate" || m_viewport->isIsolated()));
    if (id.startsWith("inspect.") || id.startsWith("annotate.") || id.startsWith("select.") || id == "file.export" || id == "file.screenshot" || id == "file.save" || id == "file.saveas" || id == "file.close")
      a->setEnabled(has);
    if (id == "file.importdoc") a->setEnabled(m_doc->browse);
    // Viewer mode keeps the editing commands: they say that the file has to be saved first (isEditAction).
  }
  if (m_pinAction) m_pinAction->setEnabled(has && !m_lastMeasure.is_null());
  if (m_design) updateDesignState();
  m_browser->setViewerMode(m_doc->browse);
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

bool MainWindow::maybeSave() {
  if (m_areasReady)
    for (AreaController* area : m_areas)
      if (!area->maybeClose()) return false;  // unfinished work in an area that the user did not give up
  if (m_benchSelect) return true;  // benches run in hidden windows: a question here would pop up on the user's desktop
  if(m_doc->snapshotBusy()){statusBar()->showMessage(tr("A snapshot is being captured. Try again shortly."),4000);return false;}
  if(m_design->sketchActive() && m_design->sketch()->modified()) {
    const auto result=QMessageBox::question(this,tr("Unfinished sketch"),tr("Finish the sketch before continuing?"),QMessageBox::Save|QMessageBox::Discard|QMessageBox::Cancel);
    if(result==QMessageBox::Cancel)return false;
    if(result==QMessageBox::Save){m_design->finishSketch();statusBar()->showMessage(tr("Finish the sketch, then repeat this action."),6000);return false;}
    m_design->sketch()->end();m_doc->setRollback({});
  }
  if (!m_doc->isDirty()) return true;
  auto r = QMessageBox::question(this, tr("Unsaved changes"), tr("Save changes to %1?").arg(m_doc->path().isEmpty() ? tr("the document") : m_doc->path()),
                                 QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
  if (r == QMessageBox::Cancel) return false;
  if (r == QMessageBox::Save) {
    try {
      if (m_doc->doc.path.empty()) action("file.saveas")->trigger();
      else m_doc->save();
    } catch (const std::exception& e) {
      QMessageBox::warning(this, tr("OPAD"), QString::fromUtf8(e.what()));
      return false;
    }
    return !m_doc->isDirty();
  }
  return true;
}

void MainWindow::closeEvent(QCloseEvent* e) {
  if(m_closePending){e->ignore();return;}
  if (!m_recoveryClosed && !maybeSave()) {
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
