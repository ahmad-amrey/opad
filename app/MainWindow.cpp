#include "opad/design/feature.hpp"
#include <QPlainTextEdit>
#include <QPointer>
#include "MainWindow.hpp"

#include <QToolButton>
#include <QResizeEvent>
#include <QMoveEvent>
#include <QFileDialog>

#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QInputDialog>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>

#include <QKeyEvent>
#include <cmath>

#include "Icons.hpp"
#include "Theme.hpp"
#include "opad/geometry.hpp"
#include "I18n.hpp"
#include "opad/inspect.hpp"

#include <Bnd_Box.hxx>
#include <QElapsedTimer>

#include <algorithm>
#include <utility>
#include <set>

MainWindow::MainWindow() : m_doc(new AppDocument(this)) {
  setWindowTitle("OPAD");
  setWindowIcon(icons::appIcon());
  resize(1600, 1000);
  setMinimumSize(1280, 800);
  setAcceptDrops(true);
  applyTheme(m_settings.value("ui/dark", true).toBool());
  buildActions();
  buildMenus();
  buildCentral();
  buildRibbon();
  buildDocks();
  buildStatusBar();
  buildDesign();

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
  });
  connect(m_doc, &AppDocument::loadFinished, this, [this](bool ok, const QString&) {
    if (!ok) return;
    const auto bodies = m_doc->scene.all_bodies();
    const bool drawing = !bodies.empty() && std::all_of(bodies.begin(), bodies.end(), [this](const auto& id) { return m_doc->scene.node(id)->representation == "drawing2d"; });
    if (auto* flat = findChild<QAction*>("view.2d")) flat->setChecked(drawing);
    if (drawing) { m_viewport->standardView("top"); m_viewport->setSelectionFilter(Viewport::SelFilter::Edge); }
  });
  connect(m_doc, &AppDocument::newDocumentCreated, m_viewport, &Viewport::home);
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
  connect(m_annotations, &AnnotationsPanel::addRequested, this, [this] { toggleTool("note"); });
  connect(m_annotations, &AnnotationsPanel::resolveRequested, this, &MainWindow::deleteOp);
  connect(m_annotations, &AnnotationsPanel::restoreRequested, this, &MainWindow::restoreOp);
  connect(m_annotations, &AnnotationsPanel::styleRequested, this, &MainWindow::restyleAnnotation);
  connect(m_annotations, &AnnotationsPanel::selectNode, this, [this](const std::string& id) { onBrowserSelection({id}); m_browser->setSelectedIds({id}); });
  m_noteCards = new NoteCards(m_doc, m_viewport, this);
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
}

// ---------------------------------------------------------------- actions
QAction* MainWindow::addAction(const QString& id, const QString& text, const QString& icon, const QKeySequence& shortcut, std::function<void()> fn, bool checkable) {
  auto* a = new QAction(text, this);
  a->setObjectName(id);
  a->setData(icon);
  if (!icon.isEmpty()) a->setIcon(icons::themed(icon));
  QString saved = m_settings.value("shortcuts/" + id).toString();
  a->setShortcut(saved.isEmpty() ? shortcut : QKeySequence(saved));
  a->setCheckable(checkable);
  a->setShortcutContext(Qt::WindowShortcut);
  QString tip = text;
  tip.remove('&');
  if (!a->shortcut().isEmpty()) tip += "  (" + a->shortcut().toString(QKeySequence::NativeText) + ")";
  a->setToolTip(tip);
  connect(a, &QAction::triggered, this, [this, fn, id] {
    if (m_loadJob && !id.startsWith("file.") && !id.startsWith("panel.") && id != "view.dark") return;  // loading: workspace is locked
    guarded(fn);
  });
  m_actions << a;
  QMainWindow::addAction(a);
  return a;
}

QAction* MainWindow::action(const QString& id) const {
  for (QAction* a : m_actions)
    if (a->objectName() == id) return a;
  return nullptr;
}

void MainWindow::guarded(const std::function<void()>& fn) {
  try {
    fn();
  } catch (const std::exception& e) {
    QMessageBox::warning(this, tr("OPAD"), i18n::t(QString::fromUtf8(e.what())));
  }
}

void MainWindow::buildActions() {
  // File
  addAction("file.new", tr("&New document"), "doc", QKeySequence::New, [this] { if (maybeSave()) m_doc->newDocument(); });
  addAction("file.open", tr("&Open…"), "open", QKeySequence("Ctrl+O"), [this] {
    if (!maybeSave()) return;
    QString p = QFileDialog::getOpenFileName(this, tr("Open"), m_settings.value("ui/lastDir").toString(), tr("Design files (*.opad *.step *.stp *.dxf *.svg *.dwg *.stl *.obj);;OPAD document (*.opad);;STEP (*.step *.stp);;2D drawings (*.dxf *.svg *.dwg);;Meshes (*.stl *.obj)"));
    if (!p.isEmpty()) openPath(p);
  });
  addAction("file.import", tr("&Import…"), "import", QKeySequence("Ctrl+I"), [this] {
    QString p = QFileDialog::getOpenFileName(this, tr("Import design"), m_settings.value("ui/lastDir").toString(), tr("Design files (*.step *.stp *.dxf *.svg *.dwg *.stl *.obj)"));
    if (p.isEmpty()) return;
    m_settings.setValue("ui/lastDir", QFileInfo(p).absolutePath());
    auto ids = currentNodeIds();
    QString parent;
    if (ids.size() == 1 && m_doc->node(ids[0]) && m_doc->node(ids[0])->kind == opad::Node::Kind::Component &&
        QMessageBox::question(this, tr("Import"), tr("Import under the selected component “%1”?").arg(m_doc->nodeName(ids[0]))) == QMessageBox::Yes)
      parent = QString::fromStdString(ids[0]);
    beginLoad([this, p] { addRecent(p); m_viewport->fitWhenReady(); });
    m_doc->startImport(p, parent);
  });
  addAction("file.importdoc", tr("Export to OPAD document…"), "save", QKeySequence("Ctrl+Shift+E"), [this] {
    if (!m_doc->browse) return;
    QString src = m_settings.value("ui/lastBrowse").toString();
    if (src.isEmpty()) throw opad::Error("No STEP file is being viewed.");
    QString dest = QFileDialog::getSaveFileName(this, tr("Export to OPAD document"), QFileInfo(src).completeBaseName() + ".opad", tr("OPAD document (*.opad)"));
    if (dest.isEmpty()) return;
    // Leaves viewer mode: a full import (healing, BREP text, content keys) into a fresh document, saved on arrival.
    beginLoad([this, dest] {
      guarded([this, dest] { m_doc->saveAs(dest); addRecent(dest); });
      m_viewport->fitWhenReady();
    });
    m_doc->startImport(src);
  });
  addAction("file.save", tr("&Save"), "save", QKeySequence("Ctrl+S"), [this] {
    if (m_doc->browse) { action("file.importdoc")->trigger(); return; }  // viewer mode: saving means exporting
    if (m_doc->doc.path.empty()) action("file.saveas")->trigger();
    else m_doc->save();
  });
  addAction("file.saveas", tr("Save &As…"), "save", QKeySequence("Ctrl+Shift+S"), [this] {
    if (m_doc->browse) throw opad::Error("Browse mode shows a STEP file without a document. Use Import to create one.");
    QString p = QFileDialog::getSaveFileName(this, tr("Save document"), m_settings.value("ui/lastDir").toString(), tr("OPAD document (*.opad)"));
    if (p.isEmpty()) return;
    if (!p.endsWith(".opad", Qt::CaseInsensitive)) p += ".opad";
    m_settings.setValue("ui/lastDir", QFileInfo(p).absolutePath());
    m_doc->saveAs(p);
    addRecent(p);
  });
  addAction("file.export", tr("&Export…"), "export", QKeySequence("Ctrl+E"), [this] { exportDialog(); });
  addAction("file.screenshot", tr("Save screens&hot…"), "export", QKeySequence("Ctrl+Shift+P"), [this] { screenshot(); });
  addAction("file.close", tr("&Close document"), "close", QKeySequence("Ctrl+W"), [this] {
    if (!m_doc->hasDocument || m_doc->loading || !maybeSave()) return;
    m_viewport->clearSelection();
    m_doc->closeDocument();  // AppDocument::changed -> showDocument(false) -> the start screen
    statusBar()->showMessage(tr("Document closed"), 4000);
  });
  addAction("file.quit", tr("&Quit"), "", QKeySequence::Quit, [this] { close(); })->setMenuRole(QAction::QuitRole);

  // View
  addAction("view.fit", tr("Fit"), "fit", QKeySequence("F"), [this] { if(m_design&&m_design->sketchActive())m_design->sketch()->fitSketch();else m_viewport->fitSelection(); });  // the selection, or everything when nothing is selected
  addAction("view.fitall", tr("Fit all"), "fit", QKeySequence("Shift+F"), [this] { if(m_design&&m_design->sketchActive())m_design->sketch()->fitSketch();else m_viewport->fitAll(); });
  addAction("view.home", tr("Home"), "home", QKeySequence("H"), [this] { m_viewport->home(); });
  addAction("view.alignPlane",tr("Align view to plane"),"plane",QKeySequence("Ctrl+Alt+0"),[this] {
    if(m_design->sketchActive()) return;
    cancelTool();
    m_design->pickSketchPlane([this](opad::json,opad::Frame frame) { m_viewport->lookAt(frame,true,false); });
  });
  addAction("view.rollleft", tr("Turn 90° left"), "rollLeft", QKeySequence(), [this] { m_viewport->rollView(90); });
  addAction("view.rollright", tr("Turn 90° right"), "rollRight", QKeySequence(), [this] { m_viewport->rollView(-90); });
  for (const auto& [name, key] : std::vector<std::pair<QString, QString>>{{"top", "Ctrl+Alt+1"}, {"front", "Ctrl+Alt+2"}, {"right", "Ctrl+Alt+3"}, {"iso", "Ctrl+Alt+4"}, {"bottom", "Ctrl+Alt+5"}, {"back", "Ctrl+Alt+6"}, {"left", "Ctrl+Alt+7"}})
    addAction("view." + name, tr("View: %1").arg(name), "home", QKeySequence(key), [this, n = name] { m_viewport->standardView(n); });
  auto* flat = addAction("view.2d",tr("2D mode"),"drawing",QKeySequence("Ctrl+Alt+D"),[this]{},true);
  flat->setObjectName("view.2d");
  flat->setCheckable(true);
  connect(flat, &QAction::toggled, this, [this](bool on) {
    m_viewport->setTwoDimensional(on);
    if (m_browserOverlay && action("panel.browser")->isChecked()) { m_browserOverlay->setVisible(m_doc->hasDocument); m_browserOverlay->raise(); }
    m_homeBtn->setVisible(!on); m_rollLeft->setVisible(!on); m_rollRight->setVisible(!on); m_alignPlane->setVisible(!on);
  });
  QAction* ortho = addAction("view.ortho", tr("Orthographic"), "ortho", QKeySequence("O"), [this] {}, true);
  ortho->setChecked(true);
  connect(ortho, &QAction::toggled, this, [this](bool on) { m_viewport->setOrthographic(on); m_settings.setValue("view/orthographic",on); updateChips(); });
  QAction* shaded = addAction("view.shaded", tr("Shaded"), "shaded", QKeySequence("5"), [this] {}, true);
  QAction* edges = addAction("view.edges", tr("Shaded + edges"), "shadedEdges", QKeySequence("6"), [this] {}, true);
  QAction* wire = addAction("view.wire", tr("Wireframe"), "wireframe", QKeySequence("7"), [this] {}, true);
  auto* styleGroup = new QActionGroup(this);
  for (QAction* a : {shaded, edges, wire}) styleGroup->addAction(a);
  edges->setChecked(true);
  connect(styleGroup, &QActionGroup::triggered, this, [this, shaded, wire](QAction* a) {
    m_settings.setValue("view/style",a->objectName());
    m_viewport->setStyle(a == shaded ? Viewport::Style::Shaded : a == wire ? Viewport::Style::Wireframe : Viewport::Style::ShadedEdges);
    updateChips();
  });
  QAction* grid = addAction("view.grid", tr("Grid"), "grid", QKeySequence("G"), [this] {}, true);
  connect(grid, &QAction::toggled, this, [this](bool on) { m_viewport->setGrid(on); m_settings.setValue("view/grid",on); });
  addAction("view.gridSettings",tr("Grid settings"),"grid",QKeySequence("S"),[this] {
    auto* dialog=new QDialog(this,Qt::Tool);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle(tr("Grid settings"));
    auto* form=new QFormLayout(dialog);auto* spacing=new QDoubleSpinBox(dialog);spacing->setRange(0,100000);spacing->setDecimals(3);spacing->setSpecialValueText(tr("Automatic"));spacing->setValue(m_settings.value("view/gridSpacing",0).toDouble());
    auto* extent=new QDoubleSpinBox(dialog);extent->setRange(1,1000000);extent->setValue(m_settings.value("view/gridExtent",100).toDouble());
    form->addRow(tr("Spacing (mm)"),spacing);form->addRow(tr("Minimum extent (mm)"),extent);
    auto changed=[this,spacing,extent]{m_viewport->configureGrid(spacing->value(),extent->value());};
    connect(spacing,&QDoubleSpinBox::valueChanged,dialog,changed);connect(extent,&QDoubleSpinBox::valueChanged,dialog,changed);
    action("view.grid")->setChecked(true);dialog->show();
  });
  auto* through=addAction("select.through",tr("Select through objects"),"wireframe",QKeySequence("Alt+X"),[this]{},true);
  through->setChecked(m_settings.value("view/selectThrough",false).toBool());
  connect(through,&QAction::toggled,this,[this](bool on){m_settings.setValue("view/selectThrough",on);m_viewport->setSelectThrough(on);});
  addAction("view.isolate", tr("Isolate"), "isolate", QKeySequence("I"), [this] { m_viewport->isolate(currentNodeIds()); });
  // macOS treats any action starting with "Exit" as Quit unless its menu role is explicit.
  addAction("view.unisolate", tr("Exit isolate"), "showAll", QKeySequence("Shift+I"), [this] { m_viewport->isolate({}); })->setMenuRole(QAction::NoRole);
  addAction("view.saveview", tr("Save view…"), "home", QKeySequence(), [this] { saveNamedView(); });
  m_darkAction = addAction("view.dark", tr("&Dark theme"), "", QKeySequence(), [this] {}, true);
  // Panel toggles: always enabled, so a closed dock can be reopened even with no document.
  // Ctrl+1/2/3 belong to the workspaces (handoff), so the panels use Alt.
  addAction("panel.browser", tr("Browser"), "browse", QKeySequence("Alt+1"), [this] {}, true);
  addAction("panel.annotations", tr("Annotations"), "annotate", QKeySequence("Alt+2"), [this] {}, true);
  addAction("panel.section", tr("Section panel"), "section", QKeySequence(), [this] {}, true);
  addAction("panel.timeline", tr("Timeline"), "commit", QKeySequence("Alt+3"), [this] {}, true);
  // Workspaces: one document, one timeline; a workspace only changes the ribbon's tabs and tools.
  auto* wsGroup = new QActionGroup(this);
  wsGroup->addAction(addAction("workspace.review", tr("Review workspace"), "eye", QKeySequence("Ctrl+1"), [this] { setWorkspace(0); }, true));
  wsGroup->addAction(addAction("workspace.design", tr("Design workspace"), "component", QKeySequence("Ctrl+2"), [this] { setWorkspace(1); }, true));
  buildDesignActions();
  addAction("panel.reset", tr("Reset layout"), "restore", QKeySequence(), [this] { resetLayout(); });
  m_darkAction->setChecked(m_settings.value("ui/dark", true).toBool());
  connect(m_darkAction, &QAction::toggled, this, [this](bool on) { applyTheme(on); refreshIcons(); });
  for (const auto& [name, preset] : std::vector<std::pair<QString, Viewport::NavPreset>>{{"Fusion", Viewport::NavPreset::Fusion}, {"SolidWorks", Viewport::NavPreset::SolidWorks}, {"Onshape", Viewport::NavPreset::Onshape}, {"Blender", Viewport::NavPreset::Blender}}) {
    QAction* a = addAction("nav." + name.toLower(), tr("Navigation: %1").arg(name), "", QKeySequence(), [this, p = preset, n = name] {
      m_viewport->setNavPreset(p);
      m_settings.setValue("ui/nav", n);
      for (QAction* o : m_actions) if (o->objectName().startsWith("nav.")) o->setChecked(o->objectName() == "nav." + n.toLower());
    }, true);
    if (m_settings.value("ui/nav", "Fusion").toString() == name) a->setChecked(true);
  }
  for (const auto& [name, f, key, icon] : std::vector<std::tuple<QString, Viewport::SelFilter, QString, QString>>{{"Bodies", Viewport::SelFilter::Body, "1", "filterBodies"}, {"Faces", Viewport::SelFilter::Face, "2", "filterFaces"}, {"Edges", Viewport::SelFilter::Edge, "3", "filterEdges"}, {"Vertices", Viewport::SelFilter::Vertex, "4", "filterVertices"}}) {
    QAction* a = addAction("select." + name.toLower(), i18n::t(name), icon, QKeySequence(key), [this, ff = f, n = name] {
      m_viewport->setSelectionFilter(ff);
      if (!m_tool.id.isEmpty()) {  // mid-tool: the steps are reworded for the new filter and start over
        m_viewport->clearSelection();
        m_toolPicks.clear();
        m_toolPoints.clear();
        refreshToolUi();
      }
      for (QAction* o : m_actions) if (o->objectName().startsWith("select.")) o->setChecked(o->objectName() == "select." + n.toLower());
    }, true);
    if (f == Viewport::SelFilter::Body) a->setChecked(true);
  }

  // Inspect
  addAction("inspect.distance", tr("Distance"), "distance", QKeySequence("D"), [this] { toggleTool("distance"); }, true);
  addAction("inspect.angle", tr("Angle"), "angle", QKeySequence("A"), [this] { toggleTool("angle"); }, true);
  addAction("inspect.radius", tr("Radius"), "radius", QKeySequence("R"), [this] { toggleTool("radius"); }, true);
  addAction("inspect.bbox", tr("Bounding box"), "bbox", QKeySequence("B"), [this] { toggleTool("bbox"); }, true);
  m_pinAction = addAction("inspect.pin", tr("Pin"), "pin", QKeySequence("P"), [this] { pinMeasurement(); });
  m_pinAction->setShortcutContext(Qt::ApplicationShortcut);
  m_pinAction->setEnabled(false);
  addAction("inspect.clear", tr("Clear measurement"), "", QKeySequence("Esc"), [this] {
    if (m_design->sketchActive()) {  // the viewport did not have the focus: same as Esc in the sketch
      QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
      m_design->sketch()->sketchKey(&esc);
    } else if (m_design->escape()) {
    } else if (!m_tool.id.isEmpty()) toolEscape();
    else if (!closeTopPanel()) clearMeasurement();
  });  // Esc closes a tool panel first
  addAction("inspect.properties", tr("Properties"), "doc", QKeySequence("Ctrl+P"), [this] {
    if (m_selRefs.empty()) m_selRefs = m_viewport->selection();
    if (m_selRefs.empty()) throw opad::Error("Select something to see its properties.");
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

  // Annotate / edit
  addAction("annotate.add", tr("Note"), "annotate", QKeySequence("N"), [this] { toggleTool("note"); }, true);
  addAction("annotate.resolve", tr("Resolve note"), "check", QKeySequence("Ctrl+Return"), [this] { resolveCurrentAnnotation(); });
  QAction* notes = addAction("annotate.show", tr("Show notes"), "annotate", QKeySequence("Shift+N"), [this] {}, true);
  notes->setChecked(m_settings.value("ui/notes", true).toBool());  // m_noteCards reads the same key once the viewport exists
  connect(notes, &QAction::toggled, this, [this](bool on) { if (m_noteCards) m_noteCards->setShown(on); });
  addAction("edit.undo", tr("&Undo"), "rollLeft", QKeySequence::Undo, [this] {
    if (m_design->sketchActive()) return m_design->sketch()->undo();  // a sketch has its own history until it is finished
    if (m_design->ownsSelection() || m_design->busy()) return;
    m_doc->undo();
  });
  addAction("edit.redo", tr("&Redo"), "rollRight", QKeySequence::Redo, [this] {
    if (m_design->sketchActive()) return m_design->sketch()->redo();
    if (m_design->ownsSelection() || m_design->busy()) return;
    m_doc->redo();
  });
  connect(m_doc, &AppDocument::undoChanged, this, &MainWindow::updateUndoActions);
  addAction("edit.rename", tr("Rename"), "rename", QKeySequence("F2"), [this] {
    auto ids = currentNodeIds();
    if (!ids.empty()) m_browser->startRename(ids.front());
  });
  addAction("edit.hide", tr("Hide"), "hide", QKeySequence("V"), [this] {
    for (const auto& id : currentNodeIds()) m_doc->run("appearance", opad::json{{"target", id}, {"visible", false}});
  });
  addAction("edit.showall", tr("Unhide all"), "eye", QKeySequence("Shift+V"), [this] {
    for (const auto& [id, n] : m_doc->scene.nodes)
      if (!n.visible) m_doc->run("appearance", opad::json{{"target", id}, {"visible", true}});
  });
  addAction("edit.filter", tr("Filter objects"), "search", QKeySequence("Ctrl+F"), [this] { m_browserOverlay->reveal(); m_browser->focusFilter(); });
  addAction("edit.selectparent", tr("Select parent"), "chevronUp", QKeySequence("Ctrl+Up"), [this] { m_browser->selectParent(); });
  addAction("edit.delete", tr("Delete (tombstone)"), "delete", QKeySequence::Delete, [this] { deleteCurrent(); });
  addAction("edit.restore", tr("Restore"), "restore", QKeySequence("Shift+Del"), [this] {
    std::string id = m_timeline->currentOp();
    if (id.empty()) throw opad::Error("Select a tombstoned marker on the timeline first.");
    restoreOp(id);
  });
  addAction("edit.selecttouched", tr("Select what it touches"), "isolate", QKeySequence("T"), [this] {
    if (!m_timeline->currentOp().empty()) selectOpTargets(m_timeline->currentOp());
  });

  // Tools
  addAction("tools.commands", tr("Search commands"), "search", QKeySequence("S"), [this] {
    CommandPalette p(m_actions, this);
    p.move(mapToGlobal(QPoint(width() / 2 - 280, 180)));
    p.exec();
  });
  addAction("tools.shortcuts", tr("Keyboard shortcuts…"), "", QKeySequence("Ctrl+K"), [this] { ShortcutEditor(m_actions, this).exec(); });
  addAction("tools.author", tr("Annotation author..."), "", QKeySequence(), [this] {
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Annotation author"), tr("Name recorded on annotations and changes:"),
        QLineEdit::Normal, m_settings.value("user/name", QString::fromStdString(opad::default_author())).toString(), &ok);
    if (ok) m_settings.setValue("user/name", name.trimmed());
  });
  m_doc->setUndoLimit(m_settings.value("edit/undoDepth", 50).toInt());
  addAction("tools.undodepth", tr("Undo history…"), "", QKeySequence(), [this] {
    bool ok = false;
    const int n = QInputDialog::getInt(this, tr("Undo history"), tr("Steps kept for undo (1–1000):"), m_doc->undoLimit(), 1, 1000, 1, &ok);
    if (!ok) return;
    m_settings.setValue("edit/undoDepth", n);
    m_doc->setUndoLimit(n);
  });
  addAction("tools.cache", tr("Clear tessellation cache"), "", QKeySequence(), [this] {
    opad::json r = opad::commands::run("cache", opad::json{{"action", "clear"}});
    statusBar()->showMessage(tr("Cache cleared: %1").arg(QString::fromStdString(r["dir"].get<std::string>())), 4000);
  });
  addAction("help.about", tr("&About OPAD"), "", QKeySequence(), [this] {
    QMessageBox::about(this, tr("About OPAD"), tr("<b>OPAD %1</b><br>Git-native STEP viewer.<br>MIT licence. Built on Open CASCADE Technology and Qt.<br><br>Headless twin: <code>opad-cli</code>; Python: <code>import opad</code>.").arg(QString::fromStdString(opad::version_string())));
  });
}

void MainWindow::refreshIcons() {
  icons::clearCache();
  for (QAction* a : m_actions) {
    QString icon = a->data().toString();
    if (!icon.isEmpty()) a->setIcon(icons::themed(icon));
  }
  if (m_statusGitIcon) m_statusGitIcon->setPixmap(icons::pixmap("git", theme::current().fg2, 14, devicePixelRatioF()));
  m_doc->refresh();
}

void MainWindow::buildMenus() {
  auto add = [&](QMenu* m, std::initializer_list<const char*> ids) {
    for (const char* id : ids) {
      if (QString(id) == "-") { m->addSeparator(); continue; }
      if (QAction* a = action(id)) m->addAction(a);
    }
  };
  QMenu* file = menuBar()->addMenu(tr("&File"));
  add(file, {"file.new", "file.open", "file.import", "file.importdoc"});
  m_recentMenu = file->addMenu(tr("Recent"));
  add(file, {"-", "file.close", "-", "file.save", "file.saveas", "-", "file.export", "file.screenshot", "-", "file.quit"});
  QMenu* edit = menuBar()->addMenu(tr("&Edit"));
  add(edit, {"edit.undo", "edit.redo", "-", "edit.rename", "edit.hide", "edit.showall", "edit.filter", "edit.selectparent", "-", "annotate.add", "annotate.resolve", "annotate.show", "-", "edit.delete", "edit.restore", "edit.selecttouched", "-", "select.bodies", "select.faces", "select.edges", "select.vertices"});
  QMenu* view = menuBar()->addMenu(tr("&View"));
  add(view, {"view.fit", "view.fitall", "view.home", "view.rollleft", "view.rollright", "-", "view.top", "view.front", "view.right", "view.iso", "view.bottom", "view.back", "view.left", "-", "view.ortho", "view.shaded", "view.edges", "view.wire", "view.grid", "view.gridSettings", "select.through", "-", "view.isolate", "view.unisolate", "-", "view.saveview"});
  m_viewsMenu = view->addMenu(tr("Named views"));
  view->addSeparator();
  QMenu* nav = view->addMenu(tr("Navigation preset"));
  add(nav, {"nav.fusion", "nav.solidworks", "nav.onshape", "nav.blender"});
  add(view, {"view.dark", "-", "workspace.review", "workspace.design", "-", "panel.browser", "panel.annotations", "panel.section", "panel.timeline", "panel.reset"});
  QMenu* inspect = menuBar()->addMenu(tr("&Inspect"));
  add(inspect, {"inspect.distance", "inspect.angle", "inspect.radius", "inspect.bbox", "inspect.pin", "inspect.clear", "-", "inspect.properties", "-", "inspect.section", "inspect.flip"});
  QMenu* designMenu = menuBar()->addMenu(tr("&Design"));
  add(designMenu, {"design.sketch", "design.convertDrawing", "design.parameters", "-"});
  for (const char* group : {"create", "modify", "combine", "pattern", "body", "construct"}) {
    QMenu* sub = designMenu->addMenu(i18n::t(QString(group).left(1).toUpper() + QString(group).mid(1)));
    for (const auto& spec : opad::design::feature_specs())
      if (spec.group == group) sub->addAction(action("design." + QString::fromStdString(spec.kind)));
  }
  add(designMenu, {"-", "design.edit", "design.regenerate", "-", "design.newcomponent", "design.reparent", "design.colour", "design.opacity", "design.lock"});
  QMenu* tools = menuBar()->addMenu(tr("&Tools"));
  add(tools, {"tools.commands", "tools.shortcuts", "tools.cache"});
  QMenu* help = menuBar()->addMenu(tr("&Help"));
  add(help, {"help.about"});
  rebuildRecentMenu();
}

void MainWindow::setWorkspace(int index) { m_ribbon->setWorkspace(index); }

void MainWindow::buildRibbon() {
  m_ribbon = new RibbonBar(this);
  auto acts = [&](std::initializer_list<const char*> ids) {
    QList<QAction*> out;
    for (const char* id : ids) if (QAction* a = action(id)) out << a;
    return out;
  };
  const int review = m_ribbon->addWorkspace({tr("Review"), "eye", "Ctrl+1", tr("Look, measure, annotate. Nothing here changes geometry or structure."), tr("ops: annotation · measurement · section · view")});
  const int design = m_ribbon->addWorkspace({tr("Design"), "component", "Ctrl+2", tr("Model parts: sketches, features, parameters; arrange the assembly."), tr("ops: param · sketch · feature · edit · regen · import · reparent · appearance")});
  Workspace sketchWs{tr("Sketch"), "sketch", "", tr("Drawing a sketch. Finish sketch returns to Design."), tr("ops: sketch · edit")};
  sketchWs.contextual = true;
  m_sketchWorkspace = m_ribbon->addWorkspace(sketchWs);
  m_ribbon->addTab(review, tr("View"), {acts({"view.fit", "view.home", "view.ortho", "view.2d"}), acts({"view.shaded", "view.edges", "view.wire", "view.grid", "view.gridSettings", "select.through"}), acts({"view.isolate", "view.unisolate"})});
  m_ribbon->addTab(review, tr("Inspect"), {acts({"inspect.distance", "inspect.angle", "inspect.radius", "inspect.bbox"}), acts({"inspect.pin", "inspect.properties"}), acts({"inspect.section", "inspect.flip"})});
  m_ribbon->addTab(review, tr("Annotate"), {acts({"panel.annotations", "annotate.add", "annotate.resolve", "annotate.show"}), acts({"edit.rename", "edit.hide", "edit.showall", "view.saveview"})});
  m_ribbon->addTab(review, tr("Export"), {acts({"file.export", "file.screenshot"}), acts({"file.import", "file.save"})});
  m_ribbon->addTab(design, tr("Solid"), {acts({"design.sketch", "design.convertDrawing", "design.extrude", "design.revolve", "design.sweep", "design.loft", "design.hole", "design.pipe", "design.coil"}),
                                         acts({"design.box", "design.cylinder", "design.sphere", "design.cone", "design.torus"}), acts({"design.parameters"})});
  m_ribbon->addTab(design, tr("Modify"), {acts({"design.offset_face", "design.thicken", "design.fillet", "design.chamfer", "design.shell", "design.draft", "design.scale"}),
                                          acts({"design.combine", "design.split", "design.move", "design.remove"}),
                                          acts({"design.mirror", "design.pattern_rect", "design.pattern_circ"})});
  m_ribbon->addTab(design, tr("Construct"), {acts({"design.plane", "design.axis"}), acts({"design.parameters", "design.edit", "design.regenerate"})});
  m_ribbon->addTab(design, tr("Assemble"), {acts({"file.import", "design.newcomponent", "design.reparent"}), acts({"edit.rename", "edit.delete", "edit.restore"}),
                                            acts({"design.colour", "design.opacity", "design.lock", "edit.hide", "view.isolate"})});
  m_ribbon->addTab(design,tr("View"),{acts({"view.fit","view.home","view.2d","view.ortho"}),acts({"view.shaded","view.edges","view.wire","view.grid","view.gridSettings","select.through"})});
  m_ribbon->addTab(design, tr("Export"), {acts({"file.export", "file.screenshot"}), acts({"file.import", "file.save"})});
  m_ribbon->addTab(m_sketchWorkspace, tr("Create"), {acts({"sketch.finish", "sketch.cancel", "sketch.panel"}),
      acts({"sketch.line", "sketch.rect", "sketch.circle", "sketch.arc3", "sketch.spline", "sketch.ellipse", "sketch.slot", "sketch.polygon", "sketch.point"})});
  m_ribbon->addTab(m_sketchWorkspace, tr("Modify"), {acts({"sketch.finish", "sketch.panel"}),
      acts({"sketch.select", "sketch.trim", "sketch.fillet", "sketch.offset", "sketch.mirror", "sketch.construction", "sketch.node", "sketch.openEnds"})});
  m_ribbon->addTab(m_sketchWorkspace, tr("Constrain"), {acts({"sketch.finish", "sketch.panel", "sketch.dimension"}),
      acts({"sketch.c.horizontal", "sketch.c.vertical", "sketch.c.coincident", "sketch.c.parallel", "sketch.c.perpendicular", "sketch.c.tangent", "sketch.c.fix"})});
  m_ribbon->addTab(m_sketchWorkspace, tr("Reference"), {acts({"sketch.finish", "sketch.panel"}),
      acts({"sketch.project", "sketch.replane", "design.parameters", "view.grid", "view.gridSettings"})});
  m_ribbon->setWorkspace(m_settings.value("ui/workspace", 0).toInt() == 1 ? design : review);
  action(m_ribbon->workspace() == design ? "workspace.design" : "workspace.review")->setChecked(true);
  connect(m_ribbon, &RibbonBar::workspaceChanged, this, [this](int i) {  // from the shortcuts or the chip's list
    if (i == m_sketchWorkspace) return;  // contextual: entered and left with the sketch, never remembered
    if (m_design && m_design->sketchActive()) return m_ribbon->setWorkspace(m_sketchWorkspace);  // a sketch is open: finish it first
    m_settings.setValue("ui/workspace", i);
    action(i == 1 ? "workspace.design" : "workspace.review")->setChecked(true);
    if (i == 1 && m_doc->hasDocument && m_viewport->selectionFilter() != Viewport::SelFilter::Body) action("select.bodies")->trigger();  // Design works on bodies
    statusBar()->showMessage(tr("%1 workspace · Ctrl+1 / 2 switch workspace").arg(i == 1 ? tr("Design") : tr("Review")), 4000);
  });
  m_ribbon->setSelectFilters(acts({"select.bodies", "select.faces", "select.edges", "select.vertices"}), {"1", "2", "3", "4"});
  m_ribbon->setSearchAction(action("tools.commands"));
  QAction* settingsAction = addAction("tools.settings", tr("Settings"), "settings", QKeySequence(), [] {});
  auto* settings = new QMenu(this);
  settings->addAction(action("view.dark"));
  auto* quality = settings->addMenu(tr("Rendering quality"));
  auto* qualityGroup = new QActionGroup(quality);
  const QStringList qualities = {tr("Draft"), tr("Studio"), tr("Realistic shadows")};
  for (int i = 0; i < qualities.size(); ++i) {
    auto* a = quality->addAction(qualities[i]);
    a->setCheckable(true); qualityGroup->addAction(a);
    a->setChecked(Viewport::savedRenderQuality() == i);
    connect(a, &QAction::triggered, this, [this, i] { m_viewport->setRenderQuality(i); });
  }
  quality->setToolTipsVisible(true);
  for (auto* a : quality->actions()) a->setToolTip(tr("Ray tracing requires a compatible OpenGL driver; Studio is used when unavailable."));
  auto* background = settings->addMenu(tr("Scene background"));
  auto* backgroundGroup = new QActionGroup(background);
  const QStringList backgrounds = {tr("Theme"), tr("Studio gradient"), tr("White"), tr("Dark slate")};
  for (int i = 0; i < backgrounds.size(); ++i) {
    auto* a = background->addAction(backgrounds[i]);
    a->setCheckable(true); backgroundGroup->addAction(a);
    a->setChecked(m_settings.value("view/background", 1).toInt() == i);
    connect(a, &QAction::triggered, this, [this, i] { m_viewport->setSceneBackground(i); });
  }
  QMenu* navMenu = settings->addMenu(tr("Navigation preset"));
  for (QAction* a : m_actions) if (a->objectName().startsWith("nav.")) navMenu->addAction(a);
  settings->addSeparator();
  settings->addAction(action("panel.browser"));
  auto* autoBrowser = settings->addAction(tr("Auto-hide scene browser"));
  autoBrowser->setCheckable(true);
  autoBrowser->setChecked(m_settings.value("ui/browserAutoHide", true).toBool());
  connect(autoBrowser, &QAction::toggled, this, [this](bool on) { m_browserOverlay->setAutoHide(on); });
  settings->addAction(action("panel.annotations"));
  settings->addAction(action("panel.timeline"));
  settings->addAction(action("panel.reset"));
  settings->addSeparator();
  QMenu* langMenu = settings->addMenu(tr("Language"));
  auto* langGroup = new QActionGroup(langMenu);
  for (const i18n::Language& l : i18n::languages()) {
    QAction* a = langMenu->addAction(l.name);
    a->setCheckable(true);
    a->setChecked(l.code == i18n::current());
    langGroup->addAction(a);
    connect(a, &QAction::triggered, this, [this, code = l.code] {
      i18n::setLanguage(code);
      if (code != i18n::current()) QMessageBox::information(this, tr("Language"), tr("The language changes the next time OPAD starts."));
    });
  }
  settings->addSeparator();
  settings->addAction(action("tools.author"));
  settings->addAction(action("tools.shortcuts"));
  settings->addAction(action("tools.undodepth"));
  settings->addAction(action("tools.cache"));
  m_ribbon->setSettingsMenu(settingsAction, settings);
  auto* host = new QToolBar(tr("Ribbon"), this);
  host->setObjectName("ribbonHost");
  host->setMovable(false);
  host->setFloatable(false);
  host->setContentsMargins(0, 0, 0, 0);
  host->addWidget(m_ribbon);
  addToolBar(Qt::TopToolBarArea, host);
}

void MainWindow::buildCentral() {
  m_stack = new QStackedWidget(this);
  m_stack->setObjectName("central");
  m_empty = new EmptyState(m_stack);
  m_viewport = new Viewport(m_doc, m_stack);
  m_viewport->setSelectThrough(action("select.through")->isChecked());
  m_stack->addWidget(m_empty);
  m_stack->addWidget(m_viewport);
  setCentralWidget(m_stack);

  // Native child widgets float above the OpenGL surface: top-left chips, bottom-right measurement card.
  m_chips = new ViewportChips(m_viewport);
  m_chips->setAttribute(Qt::WA_NativeWindow);
  m_prompt = new PromptBar(m_viewport);
  m_prompt->setAttribute(Qt::WA_NativeWindow);
  m_prompt->hide();
  m_loadShade = new LoadShade(this);
  m_loadShade->hide();
  // Home button with its shortcut hint, floating at the top-left of the view cube (design: navigation cube).
  m_homeBtn = new QWidget(m_viewport);
  m_homeBtn->setAttribute(Qt::WA_NativeWindow);
  m_homeBtn->setAutoFillBackground(true);
  auto* hl = new QVBoxLayout(m_homeBtn);
  hl->setContentsMargins(0, 0, 0, 0);
  hl->setSpacing(2);
  auto* home = new QToolButton(m_homeBtn);
  home->setObjectName("vpButton");
  home->setFixedSize(30, 30);
  home->setIconSize(QSize(16, 16));
  home->setCursor(Qt::PointingHandCursor);
  home->setToolTip(tr("Home view (H)"));
  connect(home, &QToolButton::clicked, this, [this] { m_viewport->home(); });
  auto* hint = new QLabel(QStringLiteral("H"), m_homeBtn);
  hint->setObjectName("tertiary");
  hint->setFont(theme::mono(11));
  hint->setAlignment(Qt::AlignHCenter);
  hl->addWidget(home, 0, Qt::AlignHCenter);
  hl->addWidget(hint);
  // Turn-90° buttons on either side of the cube (the arc arrows of the design).
  auto rollButton = [this](const char* icon, const QString& tip, double degrees) {
    auto* b = new QToolButton(m_viewport);
    b->setAttribute(Qt::WA_NativeWindow);
    b->setObjectName("vpButton");
    b->setFixedSize(30, 30);
    b->setIconSize(QSize(18, 18));
    b->setCursor(Qt::PointingHandCursor);
    b->setToolTip(tip);
    b->setIcon(icons::icon(icon, theme::current().fg));
    connect(b, &QToolButton::clicked, this, [this, degrees] { m_viewport->rollView(degrees); });
    return b;
  };
  m_rollLeft = rollButton("rollLeft", tr("Turn the view 90° left"), 90);
  m_rollRight = rollButton("rollRight", tr("Turn the view 90° right"), -90);
  m_alignPlane = new QToolButton(m_viewport);
  m_alignPlane->setAttribute(Qt::WA_NativeWindow);
  m_alignPlane->setObjectName("vpButton");
  m_alignPlane->setFixedSize(30,30);
  m_alignPlane->setDefaultAction(action("view.alignPlane"));
  auto paintHome = [this, home] {
    QPalette pal = m_homeBtn->palette();
    pal.setColor(QPalette::Window, theme::current().vp);
    m_homeBtn->setPalette(pal);
    home->setIcon(icons::icon("home", theme::current().fg));
    m_rollLeft->setIcon(icons::icon("rollLeft", theme::current().fg));
    m_rollRight->setIcon(icons::icon("rollRight", theme::current().fg));
  };
  paintHome();
  connect(theme::notifier(), &theme::Notifier::changed, this, paintHome);
  m_viewport->installEventFilter(this);
}

void MainWindow::buildDocks() {
  // The timeline strip runs under the browser and the viewport. There is no right dock: properties,
  // annotations and section are floating tool panels over the viewport.
  setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
  setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
  setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
  setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);

  m_browser = new BrowserPanel(m_doc, this);
  m_browserOverlay = new BrowserOverlay(m_browser, m_viewport);
  connect(m_browser,&BrowserPanel::autoHideChanged,m_browserOverlay,[this](bool on){m_browserOverlay->setAutoHide(on);});
  connect(m_doc,&AppDocument::changed,m_browserOverlay,[this] { m_browserOverlay->refresh(); });
  m_browserOverlay->place();


  m_props = new PropertiesPanel(this);
  m_annotations = new AnnotationsPanel(m_doc, this);
  m_section = new SectionPanel(m_doc, this);
  m_propsPanel = new ToolPanel("properties", "body", &Tokens::fg2, tr("Properties"), m_props, 420, this);
  m_annotationsPanel = new ToolPanel("annotations", "annotate", &Tokens::amber, tr("Annotations"), m_annotations, 520, this);
  m_sectionPanel = new ToolPanel("section", "section", &Tokens::sel, tr("Section"), m_section, 420, this);
  m_toolSteps = new ToolStepsPanel(this);
  m_toolPanel = new ToolPanel("tool", "distance", &Tokens::sel, tr("Distance"), m_toolSteps, 360, this);
  m_toolPanel->setContentSizeHint([this](int width) { return m_toolSteps->preferredSize(width); });
  connect(m_toolSteps, &ToolStepsPanel::contentSizeChanged, m_toolPanel, &ToolPanel::requestContentFit);
  m_panels = {m_propsPanel, m_annotationsPanel, m_sectionPanel, m_toolPanel};
  connect(m_propsPanel, &ToolPanel::visibilityChanged, this, [this](bool on) {
    if (!on && m_propsJob) m_propsJob->cancel();  // nobody is looking at the component bbox any more
  });

  m_timeline = new TimelineWidget(m_doc, this);
  auto* bottom = m_timelineDock = new QDockWidget(tr("Timeline"), this);
  bottom->setObjectName("dock.timeline");
  bottom->setTitleBarWidget(new QWidget(bottom));  // the strip is its own header
  bottom->setFeatures(QDockWidget::NoDockWidgetFeatures);
  bottom->setWidget(m_timeline);
  bottom->setFixedHeight(48);
  addDockWidget(Qt::BottomDockWidgetArea, bottom);


  m_props->clear();

  // Bind the panel actions to the docks' own toggle actions (both directions).
  action("panel.browser")->setChecked(true);
  connect(action("panel.browser"), &QAction::triggered, this, [this](bool on) {
    m_browserOverlay->setVisible(on);
    if (on) m_browserOverlay->reveal();
  });
  bindPanel(action("panel.annotations"), m_annotationsPanel);
  bindPanel(action("panel.section"), m_sectionPanel);
  bindPanel(action("panel.timeline"), bottom);
}

void MainWindow::bindPanel(QAction* a, QDockWidget* dock) {
  if (!a || !dock) return;
  QAction* native = dock->toggleViewAction();
  a->setChecked(native->isChecked());
  // The dock's own action only reacts to being triggered, not to setChecked, so drive the dock directly.
  connect(native, &QAction::toggled, a, [a](bool on) { if (a->isChecked() != on) a->setChecked(on); });  // closed via its X
  connect(a, &QAction::triggered, dock, [dock](bool on) {
    dock->setVisible(on);
    if (on) dock->raise();
  });
}

void MainWindow::bindPanel(QAction* a, ToolPanel* panel) {
  connect(panel, &ToolPanel::visibilityChanged, a, [a](bool on) { if (a->isChecked() != on) a->setChecked(on); });
  connect(a, &QAction::triggered, panel, [this, panel](bool on) {
    if (on) openPanel(panel);
    else panel->hide();
  });
}

void MainWindow::openPanel(ToolPanel* panel) {
  int top = 186;  // below the view cube; pinned panels already there push it down, 8 px apart
  for (ToolPanel* o : m_panels) {
    if (o == panel || !o->isVisible()) continue;
    if (!o->pinned()) o->hide();
    else if (!o->userPlaced()) top = std::max(top, o->bottom() + 8);
  }
  if (!panel->isVisible()) panel->setDefaultTop(top);
  panel->anchorTo(QRect(m_viewport->mapToGlobal(QPoint(0, 0)), m_viewport->size()));
  panel->show();
  panel->raise();
}

bool MainWindow::closeTopPanel() {
  for (ToolPanel* p : m_panels)
    if (p->isVisible() && !p->pinned()) {
      p->hide();
      return true;
    }
  return false;
}

void MainWindow::resetLayout() {
  for (QDockWidget* d : {m_timelineDock}) {
    if (!d) continue;
    d->setFloating(false);
    d->show();
  }
  m_browserOverlay->show();
  m_browserOverlay->place();
  addDockWidget(Qt::BottomDockWidgetArea, m_timelineDock);
  setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
  setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
  setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
  setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);

}

void MainWindow::buildStatusBar() {
  const Tokens& t = theme::current();
  m_statusPath = new QLabel(this);
  m_statusPath->setFont(theme::mono(12));
  m_statusPath->setContentsMargins(12, 2, 4, 2);
  m_statusGitIcon = new QLabel(this);
  m_statusGitIcon->setPixmap(icons::pixmap("git", t.fg2, 14, devicePixelRatioF()));
  m_statusGit = new QLabel(this);
  m_statusGit->setTextFormat(Qt::RichText);
  m_statusHover = new QLabel(this);
  m_statusHover->setAlignment(Qt::AlignCenter);
  m_statusHover->setObjectName("tertiary");
  m_statusSel = new QLabel(this);
  m_statusUnits = new QLabel("mm", this);
  m_statusUnits->setContentsMargins(6, 2, 14, 2);
  m_statusUnits->setMinimumWidth(m_statusUnits->sizeHint().width());
  m_progress = new ProgressStrip(this);
  m_jobs = new JobRunner(m_progress, this);
  m_viewport->setJobs(m_jobs);
  statusBar()->addWidget(m_statusPath);
  statusBar()->addWidget(m_statusGitIcon);
  statusBar()->addWidget(m_statusGit);
  // Permanent: QStatusBar hides normal widgets while a temporary message shows and re-shows them after,
  // which fought with the strip's own show/hide and drew the message across the bars.
  statusBar()->addPermanentWidget(m_statusHover, 1);
  statusBar()->addPermanentWidget(m_progress, 1);
  struct Toggle { const char* id; const char* label; const char* icon; const char* key; const char* setting; bool defaultOn; };
  for(const auto& spec : {Toggle{"view.extensions","Extensions","extensions","F11","view/extensions",true},
      Toggle{"view.tracking","Tracking","tracking","F12","view/tracking",true},
      Toggle{"view.gridSnap","Grid snapping","grid","F9","view/gridSnap",false}}) {
    auto* a=addAction(spec.id,tr(spec.label),spec.icon,QKeySequence(spec.key),[] {},true);
    a->setChecked(m_settings.value(spec.setting,spec.defaultOn).toBool());
    a->setToolTip(tr(spec.label)+QString(" (%1)").arg(spec.key));
    auto apply=[this,spec](bool on) {
      m_settings.setValue(spec.setting,on);
      if(QString(spec.id)=="view.extensions") m_viewport->setExtensionTracking(on);
      else if(QString(spec.id)=="view.tracking") m_viewport->setTracking(on);
      else m_viewport->setGridSnap(on);
    };
    connect(a,&QAction::toggled,this,apply); apply(a->isChecked());
    auto* button=new QToolButton(this); button->setDefaultAction(a); button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setAccessibleName(tr(spec.label)); button->setIconSize({18,18}); button->setFixedSize(30,26);
    auto paint=[button,a,spec] {
      const auto& t=theme::current();
      a->setIcon(icons::icon(spec.icon,a->isChecked()?t.onsel:t.fg2));
      button->setStyleSheet(QString("QToolButton { border: 1px solid %1; border-radius: 3px; background: %2; } QToolButton:checked { background: %3; border: 2px solid %3; } QToolButton:hover { border-color: %3; }").arg(t.line.name(),t.bg2.name(),t.sel.name()));
    };
    connect(theme::notifier(),&theme::Notifier::changed,button,paint); connect(a,&QAction::toggled,button,paint); paint();
    button->setFocusPolicy(Qt::NoFocus); statusBar()->addPermanentWidget(button);
  }
  statusBar()->addPermanentWidget(m_statusSel);
  statusBar()->addPermanentWidget(m_statusUnits);
  statusBar()->setSizeGripEnabled(false);
  connect(m_jobs, &JobRunner::stripShown, this, [this](bool shown) { m_statusHover->setVisible(!shown); });  // free room for the bars
}

// ---------------------------------------------------------------- design workspace
// Feature tools come from the core's spec table (one action per kind, the form is generic); sketch tools are
// only live while a sketch is open, when the ribbon shows the contextual Sketch tab set.
void MainWindow::buildDesignActions() {
  addAction("design.convertDrawing",tr("Drawing to sketch"),"drawing",QKeySequence(),[this] { drawingToSketch(); });
  addAction("design.sketch", tr("New sketch"), "sketch", QKeySequence(), [this] { m_design->startSketch(); });
  static const std::map<std::string, const char*> kKeys = {{"extrude", "E"}, {"offset_face", "Q"}, {"move", "M"}};
  for (const auto& spec : opad::design::feature_specs()) {
    const QString kind = QString::fromStdString(spec.kind);
    const auto key = kKeys.find(spec.kind);
    QAction* a = addAction("design." + kind, i18n::t(QString::fromStdString(spec.label)), QString::fromStdString(spec.icon), key == kKeys.end() ? QKeySequence() : QKeySequence(key->second),
                           [this, kind] { m_design->startFeature(kind); });
    a->setToolTip(a->toolTip() + "\n" + i18n::t(QString::fromStdString(spec.hint)));
  }
  addAction("design.parameters", tr("Parameters"), "fx", QKeySequence("Ctrl+Shift+U"), [this] { m_design->showParameters(); });
  addAction("design.regenerate", tr("Regenerate"), "regen", QKeySequence(), [this] { m_design->regenerate(true); });
  addAction("design.edit", tr("Edit feature"), "rename", QKeySequence(), [this] {
    const std::string id = m_timeline->currentOp();
    const opad::Op* op = id.empty() ? nullptr : m_doc->doc.find_op(id);
    if (!op || (op->type != "feature" && op->type != "sketch")) throw opad::Error("Select a feature or a sketch on the timeline first (or double-click it).");
    m_design->editOp(id);
  });
  addAction("design.newcomponent", tr("New component"), "plus", QKeySequence(), [this] {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New component"), tr("Name:"), QLineEdit::Normal, tr("Component"), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    opad::json args{{"name", name.trimmed().toStdString()}};
    const auto ids = currentNodeIds();
    if (ids.size() == 1 && m_doc->node(ids[0]) && m_doc->node(ids[0])->kind == opad::Node::Kind::Component) args["parent"] = ids[0];
    m_doc->run("component", args);
  });
  addAction("design.reparent", tr("Reparent"), "reparent", QKeySequence(), [this] {
    const auto ids = currentNodeIds();
    if (ids.empty()) throw opad::Error("Select the objects to move under another component first.");
    QStringList names{tr("(document root)")};
    std::vector<std::string> targets{""};
    for (const auto& [id, n] : m_doc->scene.nodes)
      if (n.kind == opad::Node::Kind::Component && std::find(ids.begin(), ids.end(), id) == ids.end()) {
        QStringList path;
        for (const auto& p : m_doc->scene.path_to(id)) path << m_doc->nodeName(p);
        names << path.join(QString::fromUtf8(" › "));
        targets.push_back(id);
      }
    bool ok = false;
    const QString chosen = QInputDialog::getItem(this, tr("Reparent"), tr("Move under:"), names, 0, false, &ok);
    if (!ok) return;
    const std::string& parent = targets[static_cast<size_t>(names.indexOf(chosen))];
    for (const auto& id : ids) m_doc->run("reparent", opad::json{{"target", id}, {"parent", parent.empty() ? opad::json(nullptr) : opad::json(parent)}});
  });
  addAction("design.colour", tr("Colour"), "shaded", QKeySequence(), [this] {
    const auto ids = currentNodeIds();
    if (ids.empty()) throw opad::Error("Select the objects to colour first.");
    const QColor c = QColorDialog::getColor(Qt::gray, this, tr("Colour"));
    if (!c.isValid()) return;
    for (const auto& id : ids) m_doc->run("appearance", opad::json{{"target", id}, {"color", {c.redF(), c.greenF(), c.blueF()}}});
  });
  addAction("design.opacity", tr("Opacity"), "wireframe", QKeySequence(), [this] {
    const auto ids = currentNodeIds();
    if (ids.empty()) throw opad::Error("Select the objects to make see-through first.");
    bool ok = false;
    const opad::Node* n = m_doc->node(ids.front());
    const int pct = QInputDialog::getInt(this, tr("Opacity"), tr("Opacity (10–100 %):"), n ? static_cast<int>(n->opacity * 100) : 100, 10, 100, 10, &ok);
    if (!ok) return;
    for (const auto& id : ids) m_doc->run("appearance", opad::json{{"target", id}, {"opacity", pct / 100.0}});
  });
  addAction("design.lock", tr("Lock"), "lock", QKeySequence(), [this] {
    const auto ids = currentNodeIds();
    if (ids.empty()) throw opad::Error("Select the objects to lock or unlock first.");
    const opad::Node* n = m_doc->node(ids.front());
    for (const auto& id : ids) m_doc->run("appearance", opad::json{{"target", id}, {"locked", !(n && n->locked)}});
  });

  // Sketch mode.
  addAction("sketch.finish", tr("Finish sketch"), "finish", QKeySequence("Ctrl+Return"), [this] { m_design->finishSketch(); });
  addAction("sketch.panel",tr("Sketch tools"),"sketch",QKeySequence("Ctrl+Alt+S"),[this]{m_design->showSketchPanel();});
  addAction("sketch.replane",tr("Redefine sketch plane"),"plane",QKeySequence(),[this]{m_design->redefineSketchPlane();});
  addAction("sketch.cancel", tr("Cancel sketch"), "close", QKeySequence(), [this] { m_design->cancelSketch(); });
  addAction("sketch.node",tr("Spline node weights"),"spline",QKeySequence("Alt+W"),[this]{m_design->sketch()->editSplineNode();});
  addAction("sketch.openEnds",tr("Find open ends"),"point",QKeySequence("Alt+E"),[this]{m_design->sketch()->findOpenVertices();});
  auto* tools = new QActionGroup(this);
  for (const auto& [tool, text, icon] : std::vector<std::tuple<QString, QString, QString>>{
           {"select", tr("Select"), "cursor"}, {"line", tr("Line"), "line"}, {"rect", tr("Rectangle"), "rect"}, {"crect", tr("Centre rectangle"), "crect"},
           {"circle", tr("Circle"), "circle"}, {"circle3", tr("3-point circle"), "circle3"}, {"arc3", tr("3-point arc"), "arc3"}, {"arcc", tr("Centre arc"), "arcc"},
           {"polygon", tr("Polygon"), "polygon"}, {"slot", tr("Slot"), "slot"}, {"ellipse", tr("Ellipse"), "ellipse"}, {"spline", tr("Spline"), "spline"},
           {"point", tr("Point"), "point"}, {"fillet", tr("Sketch fillet"), "fillet"}, {"trim", tr("Trim"), "trim"}, {"mirror", tr("Mirror"), "mirror"},
           {"offset", tr("Offset"), "offset"}, {"project", tr("Project"), "project"},
           {"dimension", tr("Dimension"), "dimension"}, {"c:horizontal", tr("Horizontal"), "cHorizontal"}, {"c:vertical", tr("Vertical"), "cVertical"},
           {"c:coincident", tr("Coincident"), "cCoincident"}, {"c:parallel", tr("Parallel"), "cParallel"}, {"c:perpendicular", tr("Perpendicular"), "cPerpendicular"},
           {"c:tangent", tr("Tangent"), "cTangent"}, {"c:equal", tr("Equal"), "cEqual"}, {"c:concentric", tr("Concentric"), "cConcentric"}, {"c:midpoint", tr("Midpoint"), "cMidpoint"},
           {"c:symmetric", tr("Symmetric"), "cSymmetric"}, {"c:collinear", tr("Collinear"), "cCollinear"}, {"c:fix", tr("Fix"), "cFix"}}) {
    QAction* a = addAction("sketch." + QString(tool).replace(':', '.'), text, icon, QKeySequence(), [this, t = tool] { m_design->sketch()->setTool(t); }, true);
    a->setProperty("sketchTool", tool);
    tools->addAction(a);
  }
  addAction("sketch.construction", tr("Construction"), "construction", QKeySequence(), [this] { m_design->sketch()->toggleConstruction(); });
}

void MainWindow::buildDesign() {
  m_design = new DesignController(m_doc, m_viewport, m_jobs, this);
  m_featurePanel = new ToolPanel("feature", "extrude", &Tokens::sel, tr("Feature"), m_design->featurePanel(), 560, this);
  m_panels << m_featurePanel;
  m_design->setPanel(m_featurePanel, [this](ToolPanel* p) { openPanel(p); });
  auto* parameters=m_design->parametersWidget();
  auto* parametersPanel=new ToolPanel("parameters","fx",&Tokens::sel,tr("Parameters"),parameters,480,this);
  m_panels<<parametersPanel;m_design->setParametersPanel(parametersPanel);
  connect(parameters,&ParametersDialog::closeRequested,parametersPanel,&ToolPanel::hide);
  auto* sketchContent=new SketchPanel(m_design->sketch(),this);
  auto* sketchPanel=new ToolPanel("sketch","sketch",&Tokens::sel,tr("Sketch tools"),sketchContent,620,this);
  m_panels<<sketchPanel;
  m_design->setSketchPanel(sketchPanel);
  sketchPanel->setEscapeHandler([this]{m_design->sketch()->stepBack();});
  connect(sketchContent,&SketchPanel::finishRequested,this,[this]{m_design->finishSketch();});
  m_panels<<m_design->planePanel();

  connect(m_design, &DesignController::status, this, [this](const QString& text) { m_statusHover->setText(text); });
  connect(m_design, &DesignController::failed, this, [this](const QString& error) { QMessageBox::warning(this, tr("OPAD"), i18n::t(error)); });
  connect(m_design, &DesignController::stateChanged, this, &MainWindow::updateDesignState);
  connect(m_timeline, &TimelineWidget::opActivated, this, [this](const std::string& id) { guarded([&] { m_design->editOp(id); }); });
  connect(m_browser, &BrowserPanel::sketchActivated, this, [this](const std::string& id) { guarded([&] { m_design->editOp(id); }); });
  updateDesignState();
}

// Sketch mode swaps the ribbon to its own tab set and back; tool buttons follow the editor's tool.
void MainWindow::updateDesignState() {
  const bool sketching = m_design->sketchActive();
  const bool has = m_doc->hasDocument && !m_doc->browse;
  if (sketching && m_ribbon->workspace() != m_sketchWorkspace) {
    m_workspaceBeforeSketch = m_ribbon->workspace();
    m_ribbon->setWorkspace(m_sketchWorkspace);
  } else if (!sketching && m_ribbon->workspace() == m_sketchWorkspace) {
    m_ribbon->setWorkspace(m_workspaceBeforeSketch);
  }
  const QString tool = sketching ? m_design->sketch()->tool() : QString();
  for (QAction* a : m_actions) {
    const QString id = a->objectName();
    if (id.startsWith("sketch.")) {
      a->setEnabled(sketching);
      if (a->isCheckable()) a->setChecked(sketching && a->property("sketchTool").toString() == tool);
    } else if (id.startsWith("design.")) {
      a->setEnabled(has && !m_doc->loading);
    } else if (id.startsWith("select.") || id.startsWith("inspect.") || id.startsWith("annotate.")) {
      if (m_doc->hasDocument) a->setEnabled(!sketching || id == "inspect.clear");  // the left button draws while sketching
    }
  }
  action("view.alignPlane")->setEnabled(m_doc->hasDocument && !sketching && !m_doc->loading);
  if(m_design->pickingPlane()) {
    m_prompt->hide(); // The side panel guides this flow; leave the corner selector unobstructed.
  } else if(sketching) {
    m_prompt->set("sketch",tr("Sketch"),m_design->sketch()->toolSteps(),tr("Esc steps back"));
    m_prompt->show();positionOverlays();
  } else if(m_tool.id.isEmpty()) m_prompt->hide();
  m_browser->setEnabled(!sketching);
  updateUndoActions();
  if (sketching) {
    const int dof = m_design->sketch()->dof();
    m_statusSel->setText(dof == 0 ? tr("Sketch fully constrained") : tr("Sketch · %1 degrees of freedom").arg(dof));
  }
}

// ---------------------------------------------------------------- theme (F31)
void MainWindow::applyTheme(bool dark) {
  m_settings.setValue("ui/dark", dark);
  theme::apply(dark);
  if (m_viewport) m_viewport->setTokens(theme::current());
  if (m_darkAction && m_darkAction->isChecked() != dark) m_darkAction->setChecked(dark);
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
    // Viewer mode: nothing that edits the document. View state (hide, isolate, section, measure) stays.
    if (m_doc->browse && (id == "edit.rename" || id == "edit.delete" || id == "edit.restore" || id.startsWith("annotate.") || id == "view.saveview" || id == "inspect.pin")) a->setEnabled(false);
  }
  if (m_pinAction) m_pinAction->setEnabled(has && !m_lastMeasure.is_null() && !m_doc->browse);
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
}

void MainWindow::updateTitle() {
  setWindowTitle(m_doc->title());
  QString path = m_doc->hasDocument ? (m_doc->browse ? tr("viewing: ") + m_settings.value("ui/lastBrowse").toString() : (m_doc->path().isEmpty() ? tr("unsaved document") : m_doc->path())) : tr("No document");
  if (!m_doc->scene.unresolved.empty()) path += tr("   ·   %1 unresolved").arg(m_doc->scene.unresolved.size());
  m_statusPath->setText(path);
  if (!m_doc->hasDocument) m_statusHover->setText(tr("File › Open a .step or .opad file, or drop one here"));
  else if (m_statusHover->text() == tr("File › Open a .step or .opad file, or drop one here")) m_statusHover->clear();
}

void MainWindow::updateChips() {
  if (!m_chips) return;
  QString mode = m_viewport->style() == Viewport::Style::Shaded ? tr("Shaded") : m_viewport->style() == Viewport::Style::Wireframe ? tr("Wireframe") : tr("Shaded + edges");
  QString proj = m_viewport->isOrthographic() ? tr("Orthographic") : tr("Perspective");
  QString section;
  if (m_section && m_section->enabled()) {
    opad::Vec3 o = m_section->origin(), n = m_section->normal();
    int axis = std::fabs(n[0]) > 0.9 ? 0 : std::fabs(n[1]) > 0.9 ? 1 : 2;
    const char axes[] = {'X', 'Y', 'Z'};
    section = QString("Section %1 = %2 mm").arg(axes[axis]).arg(o[axis], 0, 'f', 0);
  }
  m_chips->set(mode, proj, section, m_viewport->isIsolated() ? tr("Isolated · %1 bodies").arg(m_viewport->isolatedCount()) : QString());
  positionOverlays();
}

void MainWindow::positionOverlays() {
  if (!m_viewport) return;
  if (m_browserOverlay) m_browserOverlay->place();
  m_chips->move(0, 0);
  m_chips->raise();
  m_homeBtn->adjustSize();
  m_homeBtn->move(m_viewport->width() - 200, 12);  // left of the cube's axes, level with its top
  m_homeBtn->raise();
  m_rollLeft->move(m_viewport->width() - 200, 150);  // lower-left and lower-right of the cube
  m_rollLeft->raise();
  m_rollRight->move(m_viewport->width() - 40, 150);
  m_rollRight->raise();
  m_alignPlane->move(m_viewport->width()-120,150);
  m_alignPlane->raise();
  if (m_loadShade->isVisible()) {
    QRect area = m_stack->geometry();  // the workspace: central area plus the docked panels
    for (QDockWidget* d : {m_timelineDock})
      if (d && d->isVisible() && !d->isFloating()) area |= d->geometry();
    m_loadShade->place(QRect(mapToGlobal(area.topLeft()), area.size()), m_viewport->mapToGlobal(m_viewport->rect().center()));
  }
  if (trace::enabled()) trace::log(QStringLiteral("viewport at %1,%2 size %3x%4").arg(m_viewport->mapToGlobal(QPoint(0, 0)).x()).arg(m_viewport->mapToGlobal(QPoint(0, 0)).y()).arg(m_viewport->width()).arg(m_viewport->height()));
  if (m_prompt->isVisible()) {
    m_prompt->move(std::max(8, (m_viewport->width() - m_prompt->width()) / 2), 44);
    m_prompt->raise();
  }
  const QRect vp(m_viewport->mapToGlobal(QPoint(0, 0)), m_viewport->size());
  for (ToolPanel* p : m_panels)
    if (p->isVisible()) p->anchorTo(vp);  // the panels follow the viewport's top-right corner
}

void MainWindow::resizeEvent(QResizeEvent* e) {
  QMainWindow::resizeEvent(e);
  positionOverlays();
}

void MainWindow::moveEvent(QMoveEvent* e) {
  QMainWindow::moveEvent(e);
  positionOverlays();
}

void MainWindow::setLoading(bool on) {
  m_stack->setCurrentIndex(on || m_doc->hasDocument ? 1 : 0);  // the viewport (dimmed, spinner) rather than the start page while loading
  m_viewport->setBlocked(on);
  m_loadShade->setVisible(on);
  for (QWidget* w : std::initializer_list<QWidget*>{m_browser, m_timeline, m_propsPanel, m_annotationsPanel, m_sectionPanel, m_toolPanel, m_featurePanel}) w->setEnabled(!on);
  if (on) positionOverlays();
}

bool MainWindow::eventFilter(QObject* o, QEvent* e) {
  if (o == m_viewport && (e->type() == QEvent::Resize || e->type() == QEvent::Show)) positionOverlays();
  return QMainWindow::eventFilter(o, e);
}

// ---------------------------------------------------------------- git status (F33)
void MainWindow::refreshGit() {
  const Tokens& t = theme::current();
  if (!m_doc->hasDocument || m_doc->browse || m_doc->doc.path.empty()) {
    m_statusGit->clear();
    m_statusGitIcon->hide();
    return;
  }
  // git is queried asynchronously: waiting for it here blocked the UI for up to 0.8 s per query.
  QFileInfo fi(m_doc->path());
  auto notInGit = [this, t] {
    m_statusGitIcon->show();
    m_statusGit->setText(QString("<span style='color:%1'>%2</span>").arg(t.fg3.name(), tr("not in git")));
  };
  auto* git = new QProcess(this);
  git->setWorkingDirectory(fi.absolutePath());
  connect(git, &QProcess::errorOccurred, this, [git, notInGit](QProcess::ProcessError) { git->deleteLater(); notInGit(); });
  connect(git, &QProcess::finished, this, [this, git, fi, t, notInGit](int code, QProcess::ExitStatus) {
    git->deleteLater();
    if (code != 0) { notInGit(); return; }
    const QString branch = QString::fromUtf8(git->readAllStandardOutput()).trimmed();
    auto* st = new QProcess(this);
    st->setWorkingDirectory(fi.absolutePath());
    connect(st, &QProcess::errorOccurred, this, [st](QProcess::ProcessError) { st->deleteLater(); });
    connect(st, &QProcess::finished, this, [this, st, branch, t](int, QProcess::ExitStatus) {
      st->deleteLater();
      const QString status = QString::fromUtf8(st->readAllStandardOutput()).trimmed();
      const QString state = status.isEmpty() ? QString() : status.startsWith("??") ? tr("untracked") : tr("modified");
      m_statusGitIcon->show();
      m_statusGit->setText(branch.toHtmlEscaped() + (state.isEmpty() ? QString() : QString(" <span style='color:%1'>· %2</span>").arg(t.amber.name(), state)));
    });
    st->start("git", {"status", "--porcelain", "--", fi.fileName()});
  });
  git->start("git", {"rev-parse", "--abbrev-ref", "HEAD"});
}

// ---------------------------------------------------------------- selection plumbing (F22/F25)
std::vector<std::string> MainWindow::currentNodeIds() const {
  std::vector<std::string> ids;
  for (const auto& r : m_viewport->selection())
    if (r.kind != opad::Ref::Kind::Point && std::find(ids.begin(), ids.end(), r.body) == ids.end()) ids.push_back(r.body);
  if (ids.empty()) ids = m_browser->selectedIds();
  return ids;
}

void MainWindow::onViewportSelection() {
  if (m_design->ownsSelection()) return m_design->viewportSelectionChanged();  // picks for a feature input or a sketch plane
  if (m_syncing) return;
  m_syncing = true;
  auto refs = m_viewport->selection();
  std::vector<std::string> ids;
  std::set<std::string> seen;
  for (const auto& r : refs)
    if (seen.insert(r.body).second) ids.push_back(r.body);
  m_browser->setSelectedIds(ids);
  if (m_section && m_section->picking() && !refs.empty() && refs.front().kind == opad::Ref::Kind::Face) sectionFromFace(refs.front());
  selectionMoved(refs);
  if (!m_tool.id.isEmpty()) toolPicksChanged(refs, true);
  if (refs.empty()) m_statusSel->clear();
  else m_statusSel->setText(tr("%1 selected · %2").arg(refs.size()).arg(i18n::t(opad::Ref::kind_name(refs.front().kind))));
  scheduleSelectionSync();
  m_syncing = false;
}

void MainWindow::onBrowserSelection(const std::vector<std::string>& ids) {
  if (m_syncing) return;
  m_syncing = true;
  std::vector<opad::Ref> refs;
  for (const auto& id : ids) { opad::Ref r; r.body = id; refs.push_back(r); }
  selectionMoved(refs);
  if (!m_tool.id.isEmpty()) toolPicksChanged(refs, false);
  m_statusSel->setText(ids.empty() ? QString() : tr("%1 selected · body").arg(ids.size()));
  m_syncing = false;
  m_viewport->selectNodes(ids);  // sliced; selectionApplied() writes selection.json when it settles
}

// The Properties panel belongs to one selection: it is opened from the context menu (or Ctrl+P), and a new
// selection closes it. Pinned, it stays and follows the selection. Nothing is inspected while it is closed.
void MainWindow::selectionMoved(const std::vector<opad::Ref>& refs) {
  m_selRefs = refs;
  if (!m_propsPanel->isVisible()) return;
  if (m_propsPanel->pinned()) showProperties(refs);  // O(1): only the first ref is inspected and geometry walks are deferred to a job
  else m_propsPanel->hide();
}

void MainWindow::showProperties(const std::vector<opad::Ref>& refs) {
  if (refs.empty()) {
    m_propsPanel->setContext(QString());
    m_props->clear();
    return;
  }
  try {
    const opad::Ref& r = refs.front();
    const opad::Node* node = r.kind == opad::Ref::Kind::Body ? m_doc->node(r.body) : nullptr;
    const bool component = node && node->kind == opad::Node::Kind::Component;
    // A component's bbox walks every body under it, so it is filled in afterwards by a sliced job.
    opad::json j = r.kind == opad::Ref::Kind::Body ? opad::node_properties(m_doc->doc, m_doc->scene, r.body, !component) : opad::inspect_ref(m_doc->doc, m_doc->scene, r);
    QString title, subtitle, id;
    if (r.kind == opad::Ref::Kind::Point) {
      title = tr("Point");
      subtitle = QString::fromStdString(r.str());
    } else if (r.kind == opad::Ref::Kind::Body) {
      const opad::Node* n = m_doc->node(r.body);
      title = m_doc->nodeName(r.body);
      QStringList path;
      for (const auto& p : m_doc->scene.path_to(r.body)) path << m_doc->nodeName(p);
      subtitle = path.join(QString::fromUtf8(" › "));
      if (n && n->kind == opad::Node::Kind::Body) {
        auto it = m_doc->scene.instance_count.find(n->body_key);
        if (it != m_doc->scene.instance_count.end() && it->second > 1) subtitle += tr(" · %1 instances").arg(it->second);
      }
      id = QString::fromStdString(r.body.substr(0, 8));
    } else {
      QString kind = i18n::t(opad::Ref::kind_name(r.kind));
      QString geo = QString::fromStdString(j.value("surface", j.value("curve", std::string())));
      title = QString::fromUtf8("%1%2%3").arg(kind.left(1).toUpper() + kind.mid(1), geo.isEmpty() ? QString() : QString::fromUtf8(" · "), geo);
      subtitle = QString::fromUtf8("%1 › %2 %3").arg(m_doc->nodeName(r.body), kind).arg(r.index);
      id = QString::fromStdString(r.body.substr(0, 8));
    }
    if (refs.size() > 1) subtitle += tr("  (+%1 more)").arg(refs.size() - 1);
    m_propsPanel->setContext(r.kind == opad::Ref::Kind::Body || r.kind == opad::Ref::Kind::Point ? title : subtitle);
    m_props->showEntity(title, subtitle, id, j);
    if (component) showComponentBbox(r.body, title, subtitle, id, j);
  } catch (const std::exception& e) {
    m_props->showEntity(tr("Error"), QString::fromUtf8(e.what()), QString(), opad::json::object());
  }
}

// The live selection is published for agents (F25): opad-cli selection / opad.run("selection").
void MainWindow::writeSelectionFile() {
  if (m_selFileJob) m_selFileJob->cancel();
  if (m_doc->browse) return;  // viewer mode: no agent channel
  struct State {
    std::vector<opad::Ref> refs;
    opad::json sel = opad::json::array();
    size_t i = 0;
  };
  auto st = std::make_shared<State>();
  st->refs = m_viewport->selection();
  const size_t kDetailCap = 200;  // inspect geometry for at most this many; the rest are listed by ref only
  m_selFileJob = m_jobs->sliced(tr("Publishing selection"), [this, st, kDetailCap](Job&) {
    if (st->i >= st->refs.size()) return false;
    const opad::Ref& r = st->refs[st->i];
    opad::json e;
    e["ref"] = r.str();
    e["node"] = m_doc->nodeName(r.body).toStdString();
    if (st->i < kDetailCap) {
      try {
        opad::json info;
        if (r.kind == opad::Ref::Kind::Body) {
          // Bodies get O(1) descriptors: volume/area need exact integration (seconds for a heavy body),
          // so agents ask `inspect` for those on demand. Faces/edges are cheap to inspect fully.
          info = opad::node_properties(m_doc->doc, m_doc->scene, r.body, false);
          Bnd_Box b = opad::node_world_bbox(m_doc->doc, m_doc->scene, r.body);
          if (!b.IsVoid()) {
            double x0, y0, z0, x1, y1, z1;
            b.Get(x0, y0, z0, x1, y1, z1);
            info["bbox"] = {{"min", {x0, y0, z0}}, {"max", {x1, y1, z1}}, {"size", {x1 - x0, y1 - y0, z1 - z0}}, {"center", {(x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2}}};
          }
        } else {
          info = opad::inspect_ref(m_doc->doc, m_doc->scene, r);
        }
        for (const char* k : {"type", "surface", "curve", "bbox", "normal", "axis", "radius", "center", "area", "volume", "length", "key"})
          if (info.contains(k)) e[k] = info[k];
      } catch (const std::exception&) {
      }
    }
    st->sel.push_back(std::move(e));
    return ++st->i < st->refs.size();
  }, [this, st](bool completed) {
    m_selFileJob = nullptr;
    if (!completed) return;  // a newer selection superseded this one
    try {
      opad::json j;
      j["pid"] = static_cast<long long>(QCoreApplication::applicationPid());
      j["document"] = m_doc->path().toStdString();
      j["browse"] = m_doc->browse;
      j["ts"] = opad::now_iso8601();
      j["selection"] = std::move(st->sel);
      opad::write_text_file(opad::cache_dir() / "selection.json", j.dump(2));
    } catch (const std::exception&) {
    }
  });
}

void MainWindow::showContextMenu(const QPoint& globalPos, std::vector<std::string> ids) {
  if(m_design->sketchActive()) {
    QMenu menu(this);
    for(const char* id:{"sketch.construction","sketch.dimension","sketch.c.horizontal","sketch.c.vertical","sketch.c.coincident","sketch.c.tangent","sketch.c.fix","sketch.node","sketch.openEnds"})menu.addAction(action(id));
    menu.addSeparator();menu.addAction(tr("Driving / reference"),m_design->sketch(),&SketchEditor::toggleReference);
    menu.addAction(tr("Delete"),m_design->sketch(),&SketchEditor::deleteSelection);
    menu.exec(globalPos);return;
  }
  QMenu menu(this);
  auto add = [&](const char* id) { if (QAction* a = action(id); a && (!m_doc->browse || a->isEnabled())) menu.addAction(a); };  // viewer mode: editing entries are not offered
  if (!ids.empty()) {
    menu.addSection(ids.size() == 1 ? m_doc->nodeName(ids.front()) : tr("%1 objects").arg(ids.size()));
    QAction* fit = menu.addAction(icons::themed("fit", 16), tr("Fit to"));
    connect(fit, &QAction::triggered, this, [this, ids] { m_viewport->fitNodes(ids); });
    add("design.convertDrawing");
    auto* exportObject=menu.addAction(icons::themed("export",16),tr("Export selected objects"));
    connect(exportObject,&QAction::triggered,this,[this,ids] { guarded([&] { exportDialog(ids); }); });
    add("edit.selectparent");
    add("view.isolate");
    QAction* hideOthers = menu.addAction(icons::themed("hide", 16), tr("Hide others"));
    connect(hideOthers, &QAction::triggered, this, [this, ids] {
      std::set<std::string> keep;
      for (const auto& id : ids) for (const auto& b : m_doc->scene.bodies_under(id)) keep.insert(b);
      for (const auto& b : m_doc->scene.all_bodies())
        if (!keep.count(b) && m_doc->node(b)->visible) m_doc->run("appearance", opad::json{{"target", b}, {"visible", false}});
    });
    add("edit.hide");
    add("edit.rename");
    QAction* color = menu.addAction(icons::themed("dot", 16), tr("Colour…"));
    color->setVisible(!m_doc->browse);
    connect(color, &QAction::triggered, this, [this, ids] {
      QColor c = QColorDialog::getColor(Qt::gray, this, tr("Colour"));
      if (!c.isValid()) return;
      for (const auto& id : ids) m_doc->run("appearance", opad::json{{"target", id}, {"color", {c.redF(), c.greenF(), c.blueF()}}});
    });
    const opad::Node* n = m_doc->node(ids.front());
    QAction* lock = menu.addAction(icons::themed("lock", 16), n && n->locked ? tr("Unlock") : tr("Lock"));
    lock->setVisible(!m_doc->browse);
    connect(lock, &QAction::triggered, this, [this, ids, locked = n && n->locked] {
      for (const auto& id : ids) m_doc->run("appearance", opad::json{{"target", id}, {"locked", !locked}});
    });
    menu.addSeparator();
    add("annotate.add");
    add("inspect.distance");
    add("inspect.radius");
    add("inspect.properties");
    menu.addSeparator();
    QAction* del = menu.addAction(icons::themed("delete", 16), tr("Delete (tombstone import)"));
    del->setVisible(!m_doc->browse);
    connect(del, &QAction::triggered, this, [this, ids] {
      std::set<std::string> ops;
      for (const auto& id : ids) if (const opad::Node* nn = m_doc->node(id)) ops.insert(nn->source_op);
      for (const auto& op : ops) deleteOp(op);
    });
  } else {
    add("view.fit");
    add("view.home");
    add("view.unisolate");
    add("edit.showall");
    menu.addSeparator();
    add("file.import");
  }
  menu.exec(globalPos);
}

void MainWindow::timelineMenu(const std::string& requestedId, const QPoint& globalPos) {
  const std::string opId=requestedId;
  const auto generation=m_doc->generation;
  bool deleted = std::find(m_doc->scene.deleted_ops.begin(), m_doc->scene.deleted_ops.end(), opId) != m_doc->scene.deleted_ops.end();
  QMenu menu(this);
  menu.setFixedWidth(232);
  QAction* del = menu.addAction(icons::themed("delete", 16), tr("Delete (tombstone)\tDel"));
  del->setEnabled(!deleted);
  QAction* restore = menu.addAction(icons::themed("restore", 16), tr("Restore\tShift+Del"));
  restore->setEnabled(deleted);
  const opad::Op* menuOp = m_doc->doc.find_op(opId);
  const bool designOp = menuOp && (menuOp->type == "feature" || menuOp->type == "sketch") && !deleted;
  const opad::Feature* feat = m_doc->scene.feature(opId);
  const bool suppressed=feat && feat->suppressed;
  QAction* editOp = designOp ? menu.addAction(icons::themed("rename", 16), menuOp->type == "sketch" ? tr("Edit sketch") : tr("Edit feature")) : nullptr;
  QAction* suppress = designOp && feat ? menu.addAction(icons::themed(feat->suppressed ? "eye" : "hide", 16), feat->suppressed ? tr("Unsuppress") : tr("Suppress")) : nullptr;
  QAction* exportSketch=designOp && menuOp->type=="sketch" ? menu.addAction(icons::themed("export",16),tr("Export sketch")) : nullptr;
  if (designOp) menu.addSeparator();
  QAction* sel = menu.addAction(icons::themed("isolate", 16), tr("Select what it touches\tT"));
  menu.addSeparator();
  QAction* copy = menu.addAction(icons::themed("commit", 16), tr("Copy op id\tCtrl+C"));
  QAction* log = menu.addAction(icons::themed("git", 16), tr("Show in git log"));
  QAction* chosen = menu.exec(globalPos);
  if (!chosen || generation!=m_doc->generation) return;
  if (chosen == exportSketch) exportDialog({opId});
  else if (chosen == editOp) m_design->editOp(opId);
  else if (chosen == suppress) m_design->setSuppressed(opId, !suppressed);
  else if (chosen == del) deleteOp(opId);
  else if (chosen == restore) restoreOp(opId);
  else if (chosen == sel) selectOpTargets(opId);
  else if (chosen == copy) QApplication::clipboard()->setText(QString::fromStdString(opId));
  else if (chosen == log) showOpGitLog(opId,m_doc->path());
}

void MainWindow::showOpGitLog(const std::string& opId,const QString& path) {
  auto* dialog=new QDialog(this); dialog->setObjectName("opGitLog"); dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle(tr("git log for op %1").arg(QString::fromStdString(opId.substr(0,8)))); dialog->resize(700,400);
  auto* layout=new QVBoxLayout(dialog); auto* output=new QPlainTextEdit(dialog); output->setReadOnly(true); layout->addWidget(output);
  dialog->show();
  if(path.isEmpty()) { output->setPlainText(tr("Save the document in a git repository first.")); dialog->setProperty("finished",true); return; }
  output->setPlainText(tr("Reading Git history..."));
  auto* git=new QProcess(this); const QFileInfo file(path); git->setWorkingDirectory(file.absolutePath());
  auto* timeout=new QTimer(git); timeout->setSingleShot(true);
  connect(timeout,&QTimer::timeout,git,[git] {git->setProperty("timedOut",true);git->kill();});
  connect(dialog,&QObject::destroyed,git,[git] { if(git->state()!=QProcess::NotRunning) git->kill(); });
  connect(git,&QProcess::errorOccurred,dialog,[=](QProcess::ProcessError error) {
    if(error==QProcess::FailedToStart) { output->setPlainText(git->errorString()); dialog->setProperty("finished",true); git->deleteLater(); }
  });
  connect(git,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),dialog,[=](int code,QProcess::ExitStatus status) {
    timeout->stop();
    QString text=QString::fromUtf8(git->readAllStandardOutput()).trimmed();
    if(git->property("timedOut").toBool()) text=tr("Git history timed out.");
    else if(code!=0 || status!=QProcess::NormalExit) text=QString::fromUtf8(git->readAllStandardError()).trimmed();
    else if(text.isEmpty()) text=tr("Not committed yet.");
    output->setPlainText(text); dialog->setProperty("finished",true);
  });
  connect(git,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),git,&QObject::deleteLater);
  git->start("git",{"log","--format=%h %ad %an  %s","--date=short","-S",QString::fromStdString(opId),"--",file.fileName()});
  timeout->start(10000);
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

void MainWindow::updateUndoActions() {
  QAction* u = action("edit.undo");
  QAction* r = action("edit.redo");
  if (!u || !r) return;
  u->setEnabled(m_doc->hasDocument && m_doc->canUndo());
  r->setEnabled(m_doc->hasDocument && m_doc->canRedo());
  u->setText(m_doc->canUndo() ? tr("&Undo %1").arg(m_doc->undoLabel()) : tr("&Undo"));
  r->setText(m_doc->canRedo() ? tr("&Redo %1").arg(m_doc->redoLabel()) : tr("&Redo"));
}

// ---------------------------------------------------------------- guided tools
// Start the tool, then pick: the prompt bar and the tool panel walk through the steps. The viewport accumulates
// clicks while a tool runs, so its selection *is* the ordered pick list; everything here follows from
// toolPicksChanged(). A selection made before the tool was started is taken as its first picks.
QString MainWindow::refLabel(const opad::Ref& r) const {
  QString t = m_doc->nodeName(r.body);
  if (r.kind != opad::Ref::Kind::Body) t += QString::fromUtf8(" › %1 %2").arg(i18n::t(opad::Ref::kind_name(r.kind))).arg(r.index);
  return t;
}

QList<ToolStep> MainWindow::toolSteps() const {
  const Viewport::SelFilter f = m_viewport->selectionFilter();
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
  m_design->escape();  // a feature panel or a plane pick gives way
  if (!m_tool.id.isEmpty()) cancelTool();
  static const std::map<QString, std::tuple<const char*, const char*, int>> kTools = {
      {"distance", {QT_TR_NOOP("Distance"), "distance", 2}}, {"angle", {QT_TR_NOOP("Angle"), "angle", 2}},       {"radius", {QT_TR_NOOP("Radius"), "radius", 1}},
      {"bbox", {QT_TR_NOOP("Bounding box"), "bbox", 1}},     {"note", {QT_TR_NOOP("Note"), "annotate", 1}},      {"sectionface", {QT_TR_NOOP("Section"), "section", 1}}};
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
  m_viewport->setPickAccumulate(true, id == "distance");
  for (const char* a : {"inspect.distance", "inspect.angle", "inspect.radius", "inspect.bbox", "annotate.add"})
    action(a)->setChecked(id == QString(a).section('.', 1) || (id == "note" && QString(a) == "annotate.add"));
  if (toolMeasures()) {
    m_toolPanel->setHeader(m_tool.icon, m_tool.title);
    openPanel(m_toolPanel);
  }
  if (wantFaces) {
    action("select.faces")->trigger();  // clears the picks and refreshes the prompt (see the select actions)
  } else {
    const auto before = m_viewport->selection();  // selected first, tool second still works
    if (!before.empty() && static_cast<int>(before.size()) <= m_tool.steps) toolPicksChanged(before, false);
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
  for (const char* a : {"inspect.distance", "inspect.angle", "inspect.radius", "inspect.bbox", "annotate.add"}) action(a)->setChecked(false);
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
  if (static_cast<int>(picks.size()) > m_tool.steps) {
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
  if (static_cast<int>(picks.size()) == m_tool.steps) {
    if (m_tool.id == "note") {
      guarded([this] { addAnnotation(); });
      return cancelTool();
    }
    runToolMeasure();
  }
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
    else *result = opad::measure_bbox(*document, *scene, refs);
  }, [this, run, result](bool ok, const QString& error) {
    if (run == m_toolRun) m_measureJob = nullptr;
    if (run != m_toolRun || m_tool.id.isEmpty()) return;  // the picks moved on
    if (!ok) {
      statusBar()->showMessage(i18n::t(error), 6000);
      return m_viewport->deselectLast();  // that pick does not work for this tool: ask for it again
    }
    m_lastMeasure = *result;
    m_pinAction->setEnabled(!m_doc->browse);
    m_viewport->showMeasurement(m_lastMeasure);
    refreshToolUi();
  });
}

void MainWindow::refreshToolUi() {
  if (m_tool.id.isEmpty()) return;
  const QList<ToolStep> steps = toolSteps();
  const int picked = static_cast<int>(m_toolPicks.size());
  const bool done = !m_lastMeasure.is_null();
  m_prompt->set(m_tool.icon, m_tool.title, steps, m_viewport->selectionFilter() == Viewport::SelFilter::Vertex && !done
      ? tr("Ctrl-click arc to select center · Esc back") : done ? (m_doc->browse ? tr("Esc clear · 1–4 filter") : tr("P pin · Esc clear · 1–4 filter")) : picked ? tr("Esc back · 1–4 change filter") : tr("Esc cancel · 1–4 change filter"));
  m_prompt->show();
  positionOverlays();
  if (!toolMeasures()) return;

  const Viewport::SelFilter f = m_viewport->selectionFilter();
  const QString kinds = i18n::t(f == Viewport::SelFilter::Face ? "Faces" : f == Viewport::SelFilter::Edge ? "Edges" : f == Viewport::SelFilter::Vertex ? "Vertices" : "Bodies");
  m_toolPanel->setContext(tr("%1 · %2 of %3").arg(kinds.toLower()).arg(picked).arg(m_tool.steps));
  m_toolSteps->setSteps(steps, m_toolHover);
  const QString waiting = picked < steps.size() ? steps[picked].label : tr("Measuring…");
  QString explanation = waiting;
  if (done) {
    if (m_tool.id == "distance" && m_lastMeasure.contains("anchors")) explanation = tr("Click an anchor marker to move that measurement point. Edges stay selected until Esc or Clear. Choose a preset pair below.");
    else if (m_tool.id == "distance") explanation = tr("Shortest distance between the selections. Δ = point 2 − point 1 in world axes.");
    else if (m_tool.id == "angle") explanation = tr("Directions compared at a common origin. Planar faces use their normals; curved faces use their axes.");
    else if (m_tool.id == "radius") explanation = tr("Radius from the center or cylinder axis to the surface.");
    else explanation = tr("Bounding box aligned with the world X, Y and Z axes.");
  }
  m_toolSteps->setSummary(m_tool.title, explanation, done && !m_doc->browse ? tr("unpinned") : QString());
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
  if (done) {
    const opad::json& r = m_lastMeasure;
    auto num = [](const opad::json& v, int decimals) { double n = v.get<double>(); return QString::number(std::abs(n) < 0.5 * std::pow(10.0, -decimals) ? 0.0 : n, 'f', decimals); };
    const QString unit = QString::fromStdString(r.value("unit", "mm"));
    if (r.contains("value")) rows << qMakePair(m_tool.title, QString("%1 %2").arg(num(r["value"], 3), unit));
    if (r.contains("delta"))
      for (int i = 0; i < 3; ++i) rows << qMakePair(tr("Δ%1").arg(QChar("XYZ"[i])), (r["delta"][i].get<double>() >= 0.0005 ? "+" : "") + num(r["delta"][i], 3) + " mm");
    if (r.contains("supplement")) rows << qMakePair(tr("Supplement"), num(r["supplement"], 2) + QString::fromUtf8("°"));
    if (r.contains("diameter")) rows << qMakePair(tr("Diameter"), num(r["diameter"], 3) + " mm");
    for (const char* k : {"size", "min", "max"})
      if (r.contains(k) && r[k].is_array() && r[k].size() == 3) rows << qMakePair(i18n::t(QString("bbox %1").arg(k)), QString("(%1, %2, %3) mm").arg(num(r[k][0], 3), num(r[k][1], 3), num(r[k][2], 3)));
    if (r.contains("relation") && r["relation"].is_string()) rows << qMakePair(tr("Relation"), i18n::t(QString::fromStdString(r["relation"].get<std::string>())));
  }
  for(size_t i=0;i<m_toolPicks.size();++i) {
    const auto info=m_viewport->circleInfo(m_toolPicks[i]);
    if(info.contains("diameter")) rows << qMakePair(tr("Circle %1 diameter").arg(i+1),QString::number(info["diameter"].get<double>(),'f',3)+" mm");
    if(info.contains("segments")) rows << qMakePair(tr("Circle %1 mesh segments (approximate)").arg(i+1),QString::number(info["segments"].get<int>()));
  }
  m_toolSteps->setResult(rows);
  m_toolSteps->setFooter(done, !m_doc->browse);
}

void MainWindow::pinMeasurement() {
  if (m_lastMeasure.is_null()) return;
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

void MainWindow::addAnnotation() {
  auto refs = m_viewport->selection();
  opad::Ref anchor;
  if (!refs.empty()) anchor = refs.front();
  else if (!m_browser->selectedIds().empty()) anchor.body = m_browser->selectedIds().front();
  else throw opad::Error("Select a body, face, edge or vertex to anchor the note.");
  QString where = m_doc->nodeName(anchor.body);
  if (anchor.kind != opad::Ref::Kind::Body) where += QString::fromUtf8(" › %1 %2").arg(i18n::t(opad::Ref::kind_name(anchor.kind))).arg(anchor.index);
  auto* card = new QFrame(m_viewport);
  card->setAttribute(Qt::WA_NativeWindow);
  card->setObjectName("card"); card->setFixedWidth(320);
  auto* layout = new QVBoxLayout(card);
  auto* title = new QLabel(tr("Note on %1").arg(where), card); title->setWordWrap(true); title->setTextFormat(Qt::PlainText); layout->addWidget(title);
  auto* type = new QComboBox(card);
  for (const auto& style : notes::styles()) type->addItem(i18n::t(style.label), QString::fromLatin1(style.id));
  type->setCurrentIndex(3); layout->addWidget(type);
  auto* text = new QPlainTextEdit(card); text->setPlaceholderText(tr("Write a note...")); text->setMaximumHeight(110); layout->addWidget(text);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, card); layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::rejected, card, &QObject::deleteLater);
  connect(buttons, &QDialogButtonBox::accepted, card, [this, card, anchor, text, type] {
    if (text->toPlainText().trimmed().isEmpty()) return;
    guarded([&] {
      m_doc->run("annotate", {{"anchor", anchor.str()}, {"text", text->toPlainText().trimmed().toStdString()}, {"style", type->currentData().toString().toStdString()}});
      m_noteCards->setShown(true); card->deleteLater();
    });
  });
  connect(m_doc, &AppDocument::pathChanged, card, &QObject::deleteLater);
  card->adjustSize(); card->move(std::max(8, (m_viewport->width() - card->width()) / 2), 70); card->show(); card->raise(); text->setFocus();

}

void MainWindow::resolveCurrentAnnotation() {
  std::string id = m_annotations->currentOpId();
  if (id.empty()) id = m_timeline->currentOp();
  const opad::Op* op = id.empty() ? nullptr : m_doc->doc.find_op(id);
  if (!op || (op->type != "annotation" && op->type != "measurement")) throw opad::Error("Select a note in the Annotations panel or on the timeline first.");
  deleteOp(id);
}

void MainWindow::restyleAnnotation(const std::string& opId, const std::string& style) {
  const opad::Op* op = m_doc->doc.find_op(opId);
  if (!op || (op->type != "annotation" && op->type != "measurement")) return;
  guarded([&] { m_doc->run("append", opad::json{{"op", opad::json{{"op", "edit"}, {"target", opId}, {"set", {{"style", style}}}}}}); });
}

void MainWindow::deleteOp(const std::string& requestedId) {
  const std::string opId=requestedId; // Rebuilding cards can destroy the signal sender during this operation.
  // With a design history a tombstone changes what later features produce: planned on a worker.
  if (!m_doc->scene.features.empty() || !m_doc->scene.sketches.empty()) {
    m_design->applyOps({opad::json{{"op", "delete"}, {"target", opId}}}, tr("delete"));
    return m_timeline->setCurrentOp(opId);
  }
  opad::json r = m_doc->run("delete", opad::json{{"target", opId}});
  if (r.contains("id")) m_timeline->setCurrentOp(opId);
}

void MainWindow::restoreOp(const std::string& requestedId) {
  const std::string opId=requestedId;
  // Restoring = tombstoning the delete op that targets it.
  for (const auto& op : m_doc->doc.ops)
    if (op.type == "delete" && op.data.value("target", "") == opId && !m_doc->doc.is_deleted(op.id)) {
      if (!m_doc->scene.features.empty() || !m_doc->scene.sketches.empty() || m_doc->doc.find_op(opId)->type == "feature" || m_doc->doc.find_op(opId)->type == "sketch")
        m_design->applyOps({opad::json{{"op", "delete"}, {"target", op.id}}}, tr("restore"));
      else
        m_doc->run("delete", opad::json{{"target", op.id}});
      m_timeline->setCurrentOp(opId);
      return;
    }
  throw opad::Error("That operation is not tombstoned.");
}

void MainWindow::deleteCurrent() {
  std::string id = m_timeline->currentOp();
  if (!id.empty() && m_timeline->hasFocus()) return deleteOp(id);
  std::set<std::string> ops;
  for (const auto& nid : currentNodeIds()) if (const opad::Node* n = m_doc->node(nid)) ops.insert(n->source_op);
  if (ops.empty()) {
    if (id.empty()) throw opad::Error("Select objects, or a marker on the timeline, to tombstone.");
    return deleteOp(id);
  }
  if (QMessageBox::question(this, tr("Delete"), tr("Tombstone %1 import operation(s)? History is kept; Shift+Del on the timeline restores.").arg(ops.size())) != QMessageBox::Yes) return;
  for (const auto& op : ops) deleteOp(op);
}

void MainWindow::selectOpTargets(const std::string& opId) {
  const opad::Op* op = m_doc->doc.find_op(opId);
  if (!op) return;
  m_timeline->setCurrentOp(opId);
  std::vector<std::string> ids;
  const opad::json& d = op->data;
  if (d.contains("target") && d["target"].is_string() && m_doc->node(d["target"])) ids.push_back(d["target"]);
  if (d.contains("anchor")) { try { ids.push_back(opad::Ref::from_json(d["anchor"]).body); } catch (...) {} }
  if (d.contains("refs")) for (const auto& r : d["refs"]) { try { ids.push_back(opad::Ref::from_json(r).body); } catch (...) {} }
  if (const opad::Feature* f = m_doc->scene.feature(opId))
    for (const auto& b : f->result.value("bodies", opad::json::array())) ids.push_back(b.value("id", ""));
  if (op->type == "import") {
    std::function<void(const opad::json&)> walk = [&](const opad::json& nodes) {
      for (const auto& n : nodes) { ids.push_back(n.value("id", "")); if (n.contains("children")) walk(n["children"]); }
    };
    walk(d.value("nodes", opad::json::array()));
  }
  ids.erase(std::remove_if(ids.begin(), ids.end(), [&](const std::string& id) { return !m_doc->node(id); }), ids.end());
  m_browser->setSelectedIds(ids);
  onBrowserSelection(ids);
  if (!m_propsPanel->isVisible()) return;  // pinned open: show the operation itself
  m_propsPanel->setContext(QString::fromStdString(opId.substr(0, 8)));
  m_props->showEntity(m_timeline->describe(*op), QString::fromUtf8("%1 · %2").arg(QString::fromStdString(d.value("by", "")), QString::fromStdString(d.value("ts", "")).left(16).replace('T', ' ')),
                      QString::fromStdString(opId.substr(0, 8)), d);
}

// ---------------------------------------------------------------- export (F14/F15)

void MainWindow::screenshot() {
  QString out = QFileDialog::getSaveFileName(this, tr("Save screenshot"), m_settings.value("ui/lastDir").toString(), tr("PNG image (*.png)"));
  if (out.isEmpty()) return;
  if (!out.endsWith(".png", Qt::CaseInsensitive)) out += ".png";
  QImage img = m_viewport->grabImage();
  if (img.isNull() || !img.save(out)) throw opad::Error("Screenshot failed");
  statusBar()->showMessage(tr("Saved %1").arg(out), 5000);
}

// ---------------------------------------------------------------- named views (view op)
void MainWindow::saveNamedView() {
  bool ok = false;
  QString name = QInputDialog::getText(this, tr("Save view"), tr("Name:"), QLineEdit::Normal, tr("View %1").arg(m_doc->scene.views.size() + 1), &ok);
  if (!ok || name.isEmpty()) return;
  m_doc->run("view", opad::json{{"name", name.toStdString()}, {"camera", m_viewport->cameraJson()}});
}

void MainWindow::restoreNamedView(const std::string& id) {
  for (const auto& v : m_doc->scene.views)
    if (v.id == id) m_viewport->setCameraJson(v.camera);
}

void MainWindow::rebuildViewsMenu() {
  if (!m_viewsMenu) return;
  m_viewsMenu->clear();
  for (const auto& v : m_doc->scene.views) {
    QAction* a = m_viewsMenu->addAction(icons::themed("home", 16), QString::fromStdString(v.name));
    connect(a, &QAction::triggered, this, [this, id = v.id] { restoreNamedView(id); });
  }
  if (m_viewsMenu->isEmpty()) m_viewsMenu->addAction(tr("(none saved)"))->setEnabled(false);
}

// ---------------------------------------------------------------- recent files
QStringList MainWindow::recent() const { return m_settings.value("ui/recent").toStringList(); }

void MainWindow::addRecent(const QString& path) {
  QStringList list = recent();
  list.removeAll(path);
  list.prepend(path);
  while (list.size() > 8) list.removeLast();
  m_settings.setValue("ui/recent", list);
  m_empty->setRecent(list);
  rebuildRecentMenu();
}

void MainWindow::rebuildRecentMenu() {
  if (!m_recentMenu) return;
  m_recentMenu->clear();
  for (const QString& p : recent()) {
    QAction* a = m_recentMenu->addAction(icons::themed("recent", 16), p);
    connect(a, &QAction::triggered, this, [this, p] { openPath(p); });
  }
  if (m_recentMenu->isEmpty()) m_recentMenu->addAction(tr("No recent files"))->setEnabled(false);
}

// ---------------------------------------------------------------- lifecycle
void MainWindow::openPath(const QString& path) {
  if (m_doc->loading) return;
  if (!maybeSave()) return;
  m_settings.setValue("ui/lastDir", QFileInfo(path).absolutePath());
  QString ext = QFileInfo(path).suffix().toLower();
  if (ext == "step" || ext == "stp") m_settings.setValue("ui/lastBrowse", path);
  beginLoad([this, path] { m_viewPath=QFileInfo(path).absoluteFilePath(); addRecent(path); m_viewport->fitWhenReady(); });
  m_doc->startOpen(path);
}

// ---------------------------------------------------------------- load progress
// One job spans the document worker (reading/translating/building) and the viewport's tessellation.
void MainWindow::beginLoad(std::function<void()> after) {
  if (m_loadJob) m_loadJob->cancel();
  m_afterLoad = std::move(after);
  m_loadDocDone = false;
  m_meshTotal = m_meshRemaining = 0;
  m_viewport->resetMeshing();
  m_loadJob = m_jobs->begin(tr("Loading…"), true);
  setLoading(true);
  connect(m_loadJob, &Job::cancelRequested, this, [this] {
    m_doc->cancelLoad();
    m_viewport->cancelMeshing();
  });
  connect(m_loadJob, &Job::finished, this, [this](bool ok, const QString& err) {
    m_loadJob = nullptr;
    m_afterLoad = nullptr;
    setLoading(false);
    if (!ok) {
      if (err.contains("cancel", Qt::CaseInsensitive)) statusBar()->showMessage(tr("Load cancelled"), 4000);
      else QMessageBox::warning(this, tr("OPAD"), err);
    }
    if(ok) {
      if(!m_doc->path().isEmpty()) m_viewPath=QFileInfo(m_doc->path()).absoluteFilePath();
      if((!m_benchSelect || qEnvironmentVariableIsSet("OPAD_BENCH_NAVIGATION")) && !m_viewPath.isEmpty() && m_settings.value("view/lastPath").toString()==m_viewPath) {
        try {
          const auto camera=opad::json::parse(m_settings.value("view/lastCamera").toString().toStdString());
          action("view.2d")->setChecked(m_settings.value("view/last2d",false).toBool());
          action("view.ortho")->setChecked(camera.value("projection","")=="orthographic");
          m_viewport->setCameraJson(camera);
        } catch(const std::exception&) { /* Ignore stale settings from another version. */ }
      }
    }
    if (int skipped = m_viewport->skippedCount()) statusBar()->showMessage(tr("%1 bodies were not tessellated (cancelled); reopen the file to show them").arg(skipped), 8000);
    if (m_benchSelect) QTimer::singleShot(300, this, &MainWindow::runBench);
  });
}

QString MainWindow::meshPhase() const {
  return tr("Tessellating and displaying bodies (%1 of %2)").arg(m_meshTotal - m_meshRemaining).arg(m_meshTotal);
}

void MainWindow::setLoadPhase(const QString& phase, int pct) {
  if (!m_loadJob) return;
  if (trace::enabled()) trace::log(QStringLiteral("load phase: %1 (%2%)").arg(phase).arg(pct));
  m_loadJob->setPhase(phase, pct);
  m_loadJob->setOverall(overallPercent(phase, pct));
}

// Maps a phase name + within-phase percent to an overall 0-100 across reading -> building -> tessellating.
int MainWindow::overallPercent(const QString& phase, int pct) const {
  int base = 65, span = 35;  // tessellating + displaying (last phase) by default
  if (phase.contains("Reading") || phase.contains("Opening")) { base = 0; span = 10; }
  else if (phase.contains("Translating")) { base = 10; span = 30; }
  else if (phase.contains("Building")) { base = 40; span = 15; }
  else if (phase.contains("Preparing")) { base = 55; span = 10; }
  const int within = pct < 0 ? 0 : pct;
  return base + within * span / 100;
}

// The bbox of a component walks every body under it; it is added to the panel by a sliced job.
void MainWindow::showComponentBbox(const std::string& id, const QString& title, const QString& subtitle, const QString& nid, opad::json props) {
  if (m_propsJob) m_propsJob->cancel();
  auto bodies = std::make_shared<std::vector<std::string>>(m_doc->scene.bodies_under(id));
  auto i = std::make_shared<size_t>(0);
  auto box = std::make_shared<Bnd_Box>();
  m_propsJob = m_jobs->sliced(tr("Measuring %1").arg(title), [this, bodies, i, box](Job&) {
    if (*i >= bodies->size()) return false;
    try {
      box->Add(opad::node_world_bbox(m_doc->doc, m_doc->scene, (*bodies)[*i]));
    } catch (const std::exception&) {
    }
    return ++*i < bodies->size();
  }, [this, box, title, subtitle, nid, props](bool completed) mutable {
    m_propsJob = nullptr;
    if (!completed || box->IsVoid()) return;
    double x0, y0, z0, x1, y1, z1;
    box->Get(x0, y0, z0, x1, y1, z1);
    props["bbox"] = {{"min", {x0, y0, z0}}, {"max", {x1, y1, z1}}, {"size", {x1 - x0, y1 - y0, z1 - z0}}, {"center", {(x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2}}};
    m_props->showEntity(title, subtitle, nid, props);
  });
}

// --bench-select: select every root once the load has settled, log how long the selection takes, quit.
void MainWindow::runBench() {
  if(benchTodo5())return;
  if(const auto mode=qEnvironmentVariable("OPAD_BENCH_NAVIGATION");!mode.isEmpty()) {
    if(mode=="write") {
      action("view.grid")->setChecked(true);action("view.ortho")->setChecked(false);action("view.wire")->trigger();
      auto camera=m_viewport->cameraJson();camera["target"]={10,20,30};camera["eye"]={100,120,130};camera["up"]={0,0,1};camera["scale"]=175;camera["fov_deg"]=47;
      m_viewport->setCameraJson(camera);saveLastView();m_settings.sync();
      trace::log("bench: saved view preferences PASS");QCoreApplication::exit(0);return;
    }
    const auto camera=m_viewport->cameraJson();
    if(!action("view.grid")->isChecked() || m_viewport->isOrthographic() || m_viewport->style()!=Viewport::Style::Wireframe || std::abs(camera["target"][0].get<double>()-10)>1e-6 || std::abs(camera["target"][1].get<double>()-20)>1e-6 || std::abs(camera["target"][2].get<double>()-30)>1e-6 || std::abs(camera["scale"].get<double>()-175)>1e-6 || std::abs(camera["fov_deg"].get<double>()-47)>1e-6) {
      trace::log("bench: restored view preferences FAIL: "+QString::fromStdString(camera.dump()));QCoreApplication::exit(2);return;
    }
    trace::log("bench: restored view preferences across launches PASS");
    auto once=std::make_shared<QMetaObject::Connection>();
    *once=connect(m_viewport,&Viewport::filterApplied,this,[this,once] {
      disconnect(*once);
      QTimer::singleShot(0,this,[this] {
        m_design->planePicker()->choose({{"base","yz"}});
        QTimer::singleShot(700,this,[this] {
          const auto camera=m_viewport->cameraJson();
          const double dx=camera["eye"][0].get<double>()-camera["target"][0].get<double>();
          const double dy=camera["eye"][1].get<double>()-camera["target"][1].get<double>();
          const double dz=camera["eye"][2].get<double>()-camera["target"][2].get<double>();
          if(m_design->pickingPlane() || dx<=0 || std::abs(dy)>1e-6 || std::abs(dz)>1e-6) {trace::log("bench: plane alignment FAIL: "+QString::fromStdString(camera.dump()));QCoreApplication::exit(2);return;}
          m_doc->newDocument();m_viewport->home();const auto empty=m_viewport->cameraJson();
          const double extent=m_settings.value("view/gridExtent",100.0).toDouble();
          bool covered=true;
          for(double x:{-extent,extent}) for(double y:{-extent,extent}) covered=covered && m_viewport->rect().contains(m_viewport->widgetPoint({x,y,0}));
          trace::log(covered?"bench: plane alignment and empty Home grid coverage PASS":"bench: empty Home grid coverage FAIL: "+QString::fromStdString(empty.dump()));QCoreApplication::exit(covered?0:2);
        });
      });
    });
    action("view.alignPlane")->trigger();return;
  }

  if(qEnvironmentVariableIsSet("OPAD_BENCH_REVIEW")) {
    m_doc->newDocument();
    const auto id=m_doc->run("append",{{"op",{{"op","measurement"},{"kind","distance"},{"refs",{"point/0,0,0","point/3,0,0"}},{"result",{{"value",3},{"unit","mm"}}}}}})["appended"][0].get<std::string>();
    const auto cards=m_annotations->findChildren<NoteCard*>();
    if(cards.size()!=1 || !cards[0]->note().measurement || !cards[0]->note().value.contains("3")) {QCoreApplication::exit(2);return;}
    bool removed=false;
    for(auto* button:cards[0]->findChildren<QPushButton*>()) if(button->text()==tr("Remove")) {removed=true;button->click();break;}
    if(!removed) {QCoreApplication::exit(2);return;}
    if(!m_doc->scene.measurements.empty()) {QCoreApplication::exit(2);return;}
    restoreOp(id); if(m_doc->scene.measurements.size()!=1) {QCoreApplication::exit(2);return;}
    showOpGitLog(id,{});
    auto* unsaved=findChild<QDialog*>("opGitLog");
    if(!unsaved || !unsaved->property("finished").toBool()) {QCoreApplication::exit(2);return;}
    unsaved->setObjectName("closedGitLog");unsaved->close();
    showOpGitLog(id,qEnvironmentVariable("OPAD_BENCH_REVIEW"));
    QPointer<QDialog> log=findChild<QDialog*>("opGitLog");
    auto* poll=new QTimer(this);poll->setInterval(50);
    connect(poll,&QTimer::timeout,this,[=] {
      if(!log || !log->property("finished").toBool()) return;
      const auto text=log->findChild<QPlainTextEdit*>()->toPlainText();
      trace::log("bench: review measurement remove/restore and Git log PASS: "+text);
      QCoreApplication::exit(text.isEmpty()?2:0);
    });poll->start();return;
  }

  m_benchSelect = false;
  if (const auto next=qEnvironmentVariable("OPAD_BENCH_IMPORT_NEXT"); !next.isEmpty()) {
    connect(m_doc,&AppDocument::loadFinished,this,[this](bool ok,const QString& error) {
      if (!ok) { trace::log(error); QCoreApplication::exit(2); return; }
      QTimer::singleShot(3500,this,[this] {
        const bool imported=m_doc->scene.all_bodies().size()>=2;
        m_doc->newDocument();
        const bool clean=!m_design->sketchActive() && m_lastMeasure.is_null() && m_doc->scene.all_bodies().empty();
        trace::log(QString("bench: sequential import %1; document reset %2").arg(imported).arg(clean));
        QCoreApplication::exit(imported && clean?0:2);
      });
    });
    m_doc->startImport(next); return;
  }

  if(qEnvironmentVariableIsSet("OPAD_BENCH_EXPORT_DIALOG")) { exportDialog(); QCoreApplication::exit(0); return; }
  if(qEnvironmentVariableIsSet("OPAD_BENCH_WIZARD")) { drawingToSketch(); return; }
  if(const QString shot=qEnvironmentVariable("OPAD_BENCH_STATUS");!shot.isEmpty()) {
    QTimer::singleShot(700,this,[this,shot] {
      const bool dark=theme::current().dark;
      const bool a=action("view.extensions")->isChecked(),b=action("view.tracking")->isChecked(),c=action("view.gridSnap")->isChecked();
      action("view.extensions")->setChecked(true); action("view.tracking")->setChecked(false); action("view.gridSnap")->setChecked(true);
      theme::apply(true); statusBar()->grab().save(shot+".dark.png");
      theme::apply(false); statusBar()->grab().save(shot+".light.png");
      action("view.extensions")->setChecked(a); action("view.tracking")->setChecked(b); action("view.gridSnap")->setChecked(c); theme::apply(dark);
      QCoreApplication::exit(action("file.import")->text().contains("STEP")?2:0);
    }); return;
  }
  if(const QString shot=qEnvironmentVariable("OPAD_BENCH_SCENE");!shot.isEmpty()) {
    m_viewport->standardView("top");
    QTimer::singleShot(700,this,[this,shot] { m_viewport->fitAll(); QCoreApplication::exit(m_viewport->grabImage().save(shot)?0:2); });
    return;
  }
  if (qEnvironmentVariableIsSet("OPAD_BENCH_PICKING")) {
    auto once = std::make_shared<QMetaObject::Connection>();
    *once = connect(m_viewport, &Viewport::filterApplied, this, [this, once] {
      disconnect(*once);
      QTimer::singleShot(500, this, [this] {
        startTool("distance");
        if (!m_viewport->benchPicking()) return QCoreApplication::exit(2);
        if (qEnvironmentVariableIsSet("OPAD_BENCH_ORBIT_PERF")) return QCoreApplication::exit(0);
        QTimer::singleShot(1500, this, [this] {
          const bool ok = m_toolPicks.size() == 2 && m_lastMeasure.value("kind", "") == "distance";
          trace::log(QStringLiteral("bench: picking guided distance %1: %2").arg(ok ? "PASS" : "FAIL", QString::fromStdString(m_lastMeasure.dump())));
          if (const QString shot = qEnvironmentVariable("OPAD_BENCH_UISHOT"); !shot.isEmpty()) {
            m_viewport->grabImage().save(shot + ".viewport.png");
            m_toolPanel->grab().save(shot + ".panel.png");
            m_prompt->grab().save(shot + ".prompt.png");
          }
          if (!ok) return QCoreApplication::exit(2);
          const auto center = m_toolPicks.front();
          cancelTool();
          m_viewport->selectRefs({center});
          startTool("radius");
          QTimer::singleShot(1500, this, [this] {
            const bool radiusOk = m_viewport->selectionFilter() == Viewport::SelFilter::Vertex
                && m_lastMeasure.value("kind", "") == "radius" && m_lastMeasure.contains("diameter");
            trace::log(QStringLiteral("bench: picking guided radius %1: %2").arg(radiusOk ? "PASS" : "FAIL", QString::fromStdString(m_lastMeasure.dump())));
            if (const QString shot = qEnvironmentVariable("OPAD_BENCH_UISHOT"); !shot.isEmpty()) {
              m_viewport->grabImage().save(shot + ".radius.png");
              m_toolPanel->grab().save(shot + ".radius-panel.png");
            }
            if (!radiusOk) return QCoreApplication::exit(2);
            const auto saved=m_doc->scene.measurements.size();m_pinAction->trigger();
            const bool pinned=m_doc->scene.measurements.size()==saved+1;
            trace::log(QString("bench: pin measurement %1").arg(pinned?"PASS":"FAIL"));
            if(!pinned) return QCoreApplication::exit(2);
            // Route Escape from a child of the floating measurement window.
            QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
            QCoreApplication::sendEvent(m_toolSteps,&escape);
            if(const QString shot=qEnvironmentVariable("OPAD_BENCH_UISHOT");!shot.isEmpty()) {
              m_browserOverlay->setAutoHide(true);
              QTimer::singleShot(190,this,[this,shot] { m_browserOverlay->grab().save(shot+".browser.png"); });
            }
            QTimer::singleShot(250, this, [this] {
              const bool cleared = m_viewport->selection().empty();
              trace::log(QStringLiteral("bench: picking Esc clears centers %1").arg(cleared ? "PASS" : "FAIL"));
              m_doc->newDocument();
              const bool clean=m_doc->scene.measurements.empty() && m_lastMeasure.is_null() && !m_design->sketchActive();
              trace::log(QString("bench: transient measurement reset %1").arg(clean?"PASS":"FAIL"));
              QCoreApplication::exit(cleared && clean ? 0 : 2);
            });
          });
        });
      });
    });
    m_viewport->setSelectionFilter(Viewport::SelFilter::Vertex);
    return;
  }
  // Deterministic presentation smoke check: a saved measurement JSON and output prefix.
  if (const QString source = qEnvironmentVariable("OPAD_BENCH_MEASUREMENT"); !source.isEmpty()) {
    const opad::json result = opad::json::parse(opad::read_text_file(source.toStdString()));
    startTool(QString::fromStdString(result.value("kind", "distance")));
    QTimer::singleShot(1000, this, [this, result] {
      if (const QString view = qEnvironmentVariable("OPAD_BENCH_VIEW"); !view.isEmpty()) {
        if (view == "perspective") m_viewport->setOrthographic(false);
        else m_viewport->standardView(view);
      }
      if (qEnvironmentVariable("OPAD_BENCH_THEME") == "light") {
        theme::apply(false);
        m_viewport->setTokens(theme::current());
      }
      m_lastMeasure = result;
      m_viewport->showMeasurement(result);
      refreshToolUi();
      QTimer::singleShot(300, this, [this] {
        const QString shot = qEnvironmentVariable("OPAD_BENCH_UISHOT");
        m_viewport->grabImage().save(shot + ".viewport.png");
        m_toolPanel->grab().save(shot + ".panel.png");
        QCoreApplication::quit();
      });
    });
    return;
  }
  // OPAD_BENCH_DESIGN=<png>: sketch + extrude through the design controller, dump the frame, quit.
  if (const QString shot = qEnvironmentVariable("OPAD_BENCH_DESIGN"); !shot.isEmpty()) {
    setWorkspace(1);
    m_design->bench();
    QTimer::singleShot(9000, this, [this, shot] {
      trace::log(QStringLiteral("bench: design: %1 bodies, %2 features, %3 unresolved").arg(m_doc->scene.all_bodies().size()).arg(m_doc->scene.features.size()).arg(m_doc->scene.unresolved.size()));
      m_viewport->benchDesignShot(shot);
      if (const QByteArray ui = qgetenv("OPAD_BENCH_UISHOT"); !ui.isEmpty()) grab().save(QString::fromLocal8Bit(ui));
      guarded([this] { m_doc->save(); });  // the round trip through the file, and no "unsaved changes" question on the way out
      QTimer::singleShot(500, qApp, &QCoreApplication::quit);
    });
    return;
  }
  // OPAD_BENCH_DISTANCE=<n>: time body-to-body distance between the n bodies with the most faces (every pair),
  // on a worker like the tool does. The worst case for measure_distance; compare values with opad-cli measure.
  if (const int n = qEnvironmentVariableIntValue("OPAD_BENCH_DISTANCE"); n > 1) {
    const std::vector<std::string> bodies = m_doc->scene.all_bodies();
    m_jobs->async(tr("Measuring %1").arg(tr("Distance")), [this, bodies, n](Progress) {
      std::vector<std::pair<int, std::string>> heavy;  // counting faces walks each body: on the worker
      for (const auto& b : bodies) {
        try {
          heavy.push_back({opad::subshape_count(opad::node_world_shape(m_doc->doc, m_doc->scene, b), opad::Ref::Kind::Face), b});
        } catch (const std::exception&) {
        }
      }
      std::sort(heavy.rbegin(), heavy.rend());
      heavy.resize(std::min<size_t>(heavy.size(), static_cast<size_t>(n)));
      for (size_t i = 0; i < heavy.size(); ++i)
        for (size_t k = i + 1; k < heavy.size(); ++k) {
          opad::Ref a, b;
          a.body = heavy[i].second;
          b.body = heavy[k].second;
          QElapsedTimer clock;
          clock.start();
          QString out;
          try {
            out = QString::number(opad::measure_distance(m_doc->doc, m_doc->scene, a, b)["value"].get<double>(), 'g', 15);
          } catch (const std::exception& e) {
            out = QString::fromUtf8(e.what());
          }
          trace::log(QStringLiteral("bench: distance %1 (%2 faces) <-> %3 (%4 faces) = %5 in %6 ms").arg(QString::fromStdString(a.body)).arg(heavy[i].first).arg(QString::fromStdString(b.body)).arg(heavy[k].first).arg(out).arg(clock.elapsed()));
        }
    }, [](bool, const QString&) { QCoreApplication::quit(); });
    return;
  }
  // OPAD_BENCH_SECTION=<png>: section on (Z through the model's middle), then hover and drag the plane outline's
  // handle strip through synthetic mouse events on the viewport widget: logs the hovered side and the plane's
  // origin before and after the drag, dumps the frame with the handle hovered, quits.
  if (const QString shot = qEnvironmentVariable("OPAD_BENCH_SECTION"); !shot.isEmpty()) {
    m_viewport->fitAll();
    QTimer::singleShot(1500, this, [this] { action("inspect.section")->setChecked(true); });
    QTimer::singleShot(2500, this, [this, shot] {
      auto vec = [](const opad::Vec3& v) { return QStringLiteral("(%1 %2 %3)").arg(v[0], 0, 'f', 2).arg(v[1], 0, 'f', 2).arg(v[2], 0, 'f', 2); };
      auto mouse = [this](QEvent::Type type, const QPointF& at, Qt::MouseButton button, Qt::MouseButtons held) {
        QMouseEvent e(type, at, m_viewport->mapToGlobal(at), button, held, Qt::NoModifier);
        QCoreApplication::sendEvent(m_viewport, &e);
      };
      QPointF at;
      if (!m_viewport->benchSectionHandle(at)) {
        trace::log(QStringLiteral("bench: section: no plane outline (section %1)").arg(m_viewport->sectionEnabled()));
        QCoreApplication::quit();
        return;
      }
      mouse(QEvent::MouseMove, at, Qt::NoButton, Qt::NoButton);
      const opad::Vec3 before = m_section->origin();
      trace::log(QStringLiteral("bench: section: handle at %1,%2 hover side %3 cursor %4 origin %5").arg(at.x()).arg(at.y()).arg(m_viewport->sectionHover()).arg(m_viewport->cursor().shape()).arg(vec(before)));
      QTimer::singleShot(300, this, [this, shot, at, mouse, vec, before] {
        m_viewport->grabImage().save(shot);  // hovered: outline + arrow
        mouse(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
        for (int step = 1; step <= 4; ++step) mouse(QEvent::MouseMove, at + QPointF(0, -20.0 * step), Qt::NoButton, Qt::LeftButton);
        const opad::Vec3 dragged = m_section->origin();
        mouse(QEvent::MouseButtonRelease, at + QPointF(0, -80), Qt::LeftButton, Qt::NoButton);
        const opad::Vec3 after = m_section->origin();
        const double moved = std::sqrt((after[0] - before[0]) * (after[0] - before[0]) + (after[1] - before[1]) * (after[1] - before[1]) + (after[2] - before[2]) * (after[2] - before[2]));
        trace::log(QStringLiteral("bench: section: dragged 80 px up: origin %1 -> %2 (moved %3 mm, released %4, selected %5, hover side %6)")
                       .arg(vec(before), vec(after)).arg(moved, 0, 'f', 2).arg(dragged == after).arg(m_viewport->selection().size()).arg(m_viewport->sectionHover()));
        QTimer::singleShot(300, this, [this, shot] {
          m_viewport->grabImage().save(shot.left(shot.size() - 4) + ".dragged.png");
          QCoreApplication::quit();
        });
      });
    });
    return;
  }
  // OPAD_BENCH_TOOL=<distance|angle|radius|bbox>[,faces]: walk a guided tool without a mouse. Start it, click twice
  // through the view controller, log the tool's state after each, dump the prompt bar and the panel next to
  // OPAD_BENCH_UISHOT, step back with Esc, quit.
  if (const QStringList spec = qEnvironmentVariable("OPAD_BENCH_TOOL").split(',', Qt::SkipEmptyParts); !spec.isEmpty()) {
    auto state = [this](const char* when) {
      trace::log(QStringLiteral("bench: tool '%1' %2: %3 picks, result %4").arg(m_tool.id, when).arg(m_toolPicks.size()).arg(QString::fromStdString(m_lastMeasure.dump()).left(240)));
    };
    m_viewport->fitAll();
    const QStringList second = qEnvironmentVariable("OPAD_BENCH_CLICK2", "0.38,0.62").split(',');  // where the second pick goes, as view fractions
    auto walk = [this, spec, state, second] {
    QTimer::singleShot(1500, this, [this, spec, state] { startTool(spec[0]); state("started"); });
    QTimer::singleShot(2300, this, [this, state] { m_viewport->benchClick(0.5, 0.5); QTimer::singleShot(700, this, [state] { state("after click 1"); }); });
    QTimer::singleShot(4500, this, [this, state, second] { m_viewport->benchClick(second.value(0).toDouble(), second.value(1).toDouble()); QTimer::singleShot(700, this, [state] { state("after click 2"); }); });
    QTimer::singleShot(14000, this, [this, state] {
      state("settled");
      if (const QString ui = qEnvironmentVariable("OPAD_BENCH_UISHOT"); !ui.isEmpty()) {
        m_prompt->grab().save(ui + ".prompt.png");
        m_toolPanel->grab().save(ui + ".panel.png");
        m_viewport->grabImage().save(ui + ".viewport.png");
      }
      toolEscape();
    });
    QTimer::singleShot(14800, this, [this, state] { state("after Esc"); toolEscape(); toolEscape(); state("after Esc x3"); });
    QTimer::singleShot(15500, qApp, &QCoreApplication::quit);
    };
    // On a big model the filter switch is a sliced job: picking before it has reached every body hits nothing.
    if (spec.size() > 1) {
      auto once = std::make_shared<QMetaObject::Connection>();
      *once = connect(m_viewport, &Viewport::filterApplied, this, [once, walk] { disconnect(*once); walk(); });
      action("select." + spec[1])->trigger();
    } else {
      walk();
    }
    return;
  }
  const std::vector<std::string> roots = m_doc->scene.roots;
  auto t = std::make_shared<QElapsedTimer>();
  t->start();
  trace::log(QStringLiteral("bench: selecting %1 roots (%2 bodies)").arg(roots.size()).arg(m_doc->scene.all_bodies().size()));
  auto conn = std::make_shared<QMetaObject::Connection>();
  *conn = connect(m_viewport, &Viewport::selectionApplied, this, [this, t, conn] {
    disconnect(*conn);
    trace::log(QStringLiteral("bench: selection applied after %1 ms (%2 refs)").arg(t->elapsed()).arg(m_viewport->selection().size()));
    trace::log(QStringLiteral("bench: camera before fit %1").arg(QString::fromStdString(m_viewport->cameraJson().dump())));
    m_viewport->fitSelection();
    trace::log(QStringLiteral("bench: camera after fitSelection %1").arg(QString::fromStdString(m_viewport->cameraJson().dump())));
    m_viewport->fitAll();
    m_viewport->fitNodes(m_doc->scene.bodies_under(m_doc->scene.roots.front()).size() > 1 ? std::vector<std::string>{m_doc->scene.bodies_under(m_doc->scene.roots.front()).front()} : m_doc->scene.roots);
    trace::log(QStringLiteral("bench: camera after fitNodes(first body) %1").arg(QString::fromStdString(m_viewport->cameraJson().dump())));
    // Phase 2: the deepest leaf, selected from the browser as a user would, then Fit selection.
    std::string leaf;
    size_t depth = 0;
    for (const auto& b : m_doc->scene.all_bodies()) {
      const size_t d = m_doc->scene.path_to(b).size();
      if (d > depth) { depth = d; leaf = b; }
    }
    auto conn2 = std::make_shared<QMetaObject::Connection>();
    *conn2 = connect(m_viewport, &Viewport::selectionApplied, this, [this, conn2, leaf] {
      disconnect(*conn2);
      trace::log(QStringLiteral("bench: leaf %1 selected (%2 refs)").arg(m_doc->nodeName(leaf)).arg(m_viewport->selection().size()));
      m_viewport->fitAll();
      trace::log(QStringLiteral("bench: camera after fitAll %1").arg(QString::fromStdString(m_viewport->cameraJson().dump())));
      m_viewport->fitSelection();
      trace::log(QStringLiteral("bench: camera after fitSelection(leaf) %1").arg(QString::fromStdString(m_viewport->cameraJson().dump())));
      m_viewport->benchPick();
      if (!qEnvironmentVariableIsSet("OPAD_BENCH_FILTER")) m_section->beginPick();  // then a face pick must set the section plane (logged as "section from face")
      // Undo/redo: hide the leaf, undo back to the saved state (clean again), redo.
      const size_t n0 = m_doc->doc.ops.size();
      m_doc->run("appearance", opad::json{{"target", leaf}, {"visible", false}});
      const bool d1 = m_doc->doc.dirty, u1 = m_doc->canUndo();
      m_doc->undo();
      const bool d2 = m_doc->doc.dirty;
      const size_t n2 = m_doc->doc.ops.size();
      m_doc->redo();
      trace::log(QStringLiteral("bench: undo/redo: after hide dirty=%1 canUndo=%2; after undo dirty=%3 ops %4->%5; after redo dirty=%6 ops %7 canRedo=%8").arg(d1).arg(u1).arg(d2).arg(n0).arg(n2).arg(m_doc->doc.dirty).arg(m_doc->doc.ops.size()).arg(m_doc->canRedo()));
      QTimer::singleShot(1500, this, [this] { m_viewport->fitAll(); m_viewport->benchPick(); });  // the board: a planar face at the centre
      if (const QByteArray shot = qgetenv("OPAD_BENCH_SHOT"); !shot.isEmpty()) m_viewport->benchShot(QString::fromLocal8Bit(shot));
      // The widget side of the window (ribbon, docks, splitters; the native viewport comes out blank): a UI check
      // that needs no mouse or keyboard driving. OPAD_BENCH_WORKSPACE=1 switches to Design first.
      if (qEnvironmentVariableIntValue("OPAD_BENCH_WORKSPACE") == 1) setWorkspace(1);
      if (const QByteArray ui = qgetenv("OPAD_BENCH_UISHOT"); !ui.isEmpty())
        QTimer::singleShot(300, this, [this, ui] { grab().save(QString::fromLocal8Bit(ui)); });  // after the layout has settled
      // OPAD_BENCH_FILTER=face|edge|vertex: switch the selection mode, time it, then pick a sub-shape.
      if (const QByteArray filter = qgetenv("OPAD_BENCH_FILTER"); !filter.isEmpty()) {
        QTimer::singleShot(3000, this, [this, filter] {
          auto ft = std::make_shared<QElapsedTimer>();
          ft->start();
          connect(m_viewport, &Viewport::filterApplied, this, [this, ft, filter] {
            trace::log(QStringLiteral("bench: filter %1 applied after %2 ms").arg(QString::fromLatin1(filter)).arg(ft->elapsed()));
            m_viewport->fitNodes({m_viewport->benchHeaviest()});
            m_viewport->benchPick();
            const auto refs = m_viewport->selection();
            trace::log(QStringLiteral("bench: sub-shape pick: %1 refs, kind %2 index %3").arg(refs.size()).arg(refs.empty() ? -1 : static_cast<int>(refs.front().kind)).arg(refs.empty() ? -1 : refs.front().index));
            if (const QByteArray shot = qgetenv("OPAD_BENCH_SUBSHOT"); !shot.isEmpty()) m_viewport->benchSubShot(QString::fromLocal8Bit(shot));
            if (!qEnvironmentVariableIsSet("OPAD_BENCH_BAND")) {
              QTimer::singleShot(1000, qApp, &QCoreApplication::quit);
              return;
            }
            // OPAD_BENCH_BAND: rubber band over everything in this mode, then clear it; the watchdog logs stalls.
            QTimer::singleShot(500, this, [this] {
              auto ht = std::make_shared<QElapsedTimer>();
              ht->start();
              connect(m_viewport, &Viewport::subHighlightApplied, this, [ht] { trace::log(QStringLiteral("bench: band highlight shown after %1 ms").arg(ht->elapsed())); });
              m_viewport->benchBand();
              QTimer::singleShot(6000, this, [this] {
                if (const QByteArray shot = qgetenv("OPAD_BENCH_BANDSHOT"); !shot.isEmpty()) m_viewport->grabImage().save(QString::fromLocal8Bit(shot));
                auto ct = std::make_shared<QElapsedTimer>();
                ct->start();
                connect(m_viewport, &Viewport::selectionApplied, this, [ct] {
                  trace::log(QStringLiteral("bench: band cleared after %1 ms").arg(ct->elapsed()));
                  QTimer::singleShot(1500, qApp, &QCoreApplication::quit);
                });
                m_viewport->clearSelection();
              });
            });
          });
          m_viewport->setSelectionFilter(filter == "edge" ? Viewport::SelFilter::Edge : filter == "vertex" ? Viewport::SelFilter::Vertex : Viewport::SelFilter::Face);
        });
        return;
      }
      QTimer::singleShot(4000, qApp, &QCoreApplication::quit);
    });
    m_browser->setSelectedIds({leaf});
    onBrowserSelection({leaf});
  });
  onBrowserSelection(roots);
}

void MainWindow::scheduleSelectionSync() { m_selFileTimer.start(); }

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
  if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e) {
  for (const QUrl& u : e->mimeData()->urls()) {
    QString p = u.toLocalFile();
    QString ext = QFileInfo(p).suffix().toLower();
    if (QStringList{"step", "stp", "opad", "dxf", "svg", "dwg", "stl", "obj"}.contains(ext)) { openPath(p); return; }
  }
}

bool MainWindow::maybeSave() {
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

void MainWindow::saveLastView() {
  if((m_benchSelect && !qEnvironmentVariableIsSet("OPAD_BENCH_NAVIGATION")) || m_viewPath.isEmpty() || (m_design && m_design->sketchActive())) return;
  const auto camera=m_viewport->cameraJson(); if(camera.empty()) return;
  m_settings.setValue("view/lastPath",m_viewPath);
  m_settings.setValue("view/lastCamera",QString::fromStdString(camera.dump()));
  m_settings.setValue("view/last2d",action("view.2d")->isChecked());
}

void MainWindow::closeEvent(QCloseEvent* e) {
  if (!maybeSave()) {
    e->ignore();
    return;
  }
  saveLastView();
  m_settings.setValue("ui/geometry", saveGeometry());
  if (m_timelineHiddenByViewer) m_timelineDock->show();  // viewer mode hid it; do not save that as the user's layout
  m_settings.setValue("ui/state", saveState());
  m_settings.setValue("ui/layoutVersion", 3);
  e->accept();
}
