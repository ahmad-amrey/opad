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

#include "Icons.hpp"
#include "Theme.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"

#include <Bnd_Box.hxx>
#include <QElapsedTimer>

#include <algorithm>
#include <set>

MainWindow::MainWindow() : m_doc(new AppDocument(this)) {
  setWindowTitle("OPAD");
  setWindowIcon(icons::icon("body", theme::current().sel));
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

  connect(m_doc, &AppDocument::changed, this, [this] {
    trace::Scope scope("MainWindow: document changed");
    updateTitle();
    rebuildViewsMenu();
    updateChips();
    showDocument(m_doc->hasDocument);
  });
  connect(m_doc, &AppDocument::pathChanged, this, [this] { updateTitle(); refreshGit(); });
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
  connect(m_annotations, &AnnotationsPanel::addRequested, this, &MainWindow::addAnnotation);
  connect(m_annotations, &AnnotationsPanel::resolveRequested, this, &MainWindow::deleteOp);
  connect(m_annotations, &AnnotationsPanel::restoreRequested, this, &MainWindow::restoreOp);
  connect(m_annotations, &AnnotationsPanel::selectNode, this, [this](const std::string& id) { onBrowserSelection({id}); m_browser->setSelectedIds({id}); });
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
  connect(m_section, &SectionPanel::pickRequested, this, [this] {
    if (m_viewport->selectionFilter() != Viewport::SelFilter::Face) action("select.faces")->trigger();
    statusBar()->showMessage(tr("Section: click a planar face in the 3D view to set the plane"), 6000);
  });
  connect(m_section, &SectionPanel::enabledChanged, this, [this](bool on) {
    if (action("view.section")->isChecked() != on) action("view.section")->setChecked(on);
  });
  connect(m_section, &SectionPanel::saveRequested, this, [this](const QString& name, const opad::Vec3& o, const opad::Vec3& n) {
    bool ok = false;
    QString finalName = QInputDialog::getText(this, tr("Named section"), tr("Name:"), QLineEdit::Normal, name, &ok);
    if (!ok || finalName.isEmpty()) return;
    guarded([&] { m_doc->run("section", opad::json{{"name", finalName.toStdString()}, {"origin", o}, {"normal", n}}); });
  });
  connect(m_timeline, &TimelineWidget::opClicked, this, &MainWindow::selectOpTargets);
  connect(m_timeline, &TimelineWidget::contextRequested, this, &MainWindow::timelineMenu);
  connect(m_measureCard, &MeasureCard::pinRequested, this, &MainWindow::pinMeasurement);
  connect(m_measureCard, &MeasureCard::clearRequested, this, &MainWindow::clearMeasurement);
  connect(m_empty, &EmptyState::openRequested, action("file.open"), &QAction::trigger);
  connect(m_empty, &EmptyState::importRequested, action("file.import"), &QAction::trigger);
  connect(m_empty, &EmptyState::recentChosen, this, &MainWindow::openPath);
  connect(m_empty, &EmptyState::filesDropped, this, [this](const QStringList& paths) { openPath(paths.first()); });

  m_gitTimer.setInterval(5000);
  connect(&m_gitTimer, &QTimer::timeout, this, &MainWindow::refreshGit);
  m_gitTimer.start();

  restoreGeometry(m_settings.value("ui/geometry").toByteArray());
  if (m_settings.value("ui/layoutVersion").toInt() == 2) restoreState(m_settings.value("ui/state").toByteArray());
  // restoreState carries the corner layout of older sessions; the design fixes it, so re-apply.
  setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
  setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
  setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
  setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
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
    QMessageBox::warning(this, tr("OPAD"), QString::fromUtf8(e.what()));
  }
}

void MainWindow::buildActions() {
  // File
  addAction("file.new", tr("&New document"), "doc", QKeySequence::New, [this] { if (maybeSave()) m_doc->newDocument(); });
  addAction("file.open", tr("&Open…"), "open", QKeySequence("Ctrl+O"), [this] {
    if (!maybeSave()) return;
    QString p = QFileDialog::getOpenFileName(this, tr("Open"), m_settings.value("ui/lastDir").toString(), tr("OPAD or STEP (*.opad *.step *.stp);;OPAD document (*.opad);;STEP (*.step *.stp)"));
    if (!p.isEmpty()) openPath(p);
  });
  addAction("file.import", tr("&Import STEP…"), "import", QKeySequence("Ctrl+I"), [this] {
    QString p = QFileDialog::getOpenFileName(this, tr("Import STEP"), m_settings.value("ui/lastDir").toString(), tr("STEP (*.step *.stp)"));
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
  addAction("file.quit", tr("&Quit"), "", QKeySequence::Quit, [this] { close(); });

  // View
  addAction("view.fit", tr("Fit"), "fit", QKeySequence("F"), [this] { m_viewport->fitSelection(); });  // the selection, or everything when nothing is selected
  addAction("view.fitall", tr("Fit all"), "fit", QKeySequence("Shift+F"), [this] { m_viewport->fitAll(); });
  addAction("view.home", tr("Home"), "home", QKeySequence("H"), [this] { m_viewport->home(); });
  addAction("view.rollleft", tr("Turn 90° left"), "rollLeft", QKeySequence(), [this] { m_viewport->rollView(90); });
  addAction("view.rollright", tr("Turn 90° right"), "rollRight", QKeySequence(), [this] { m_viewport->rollView(-90); });
  for (const auto& [name, key] : std::vector<std::pair<QString, QString>>{{"top", "Ctrl+1"}, {"front", "Ctrl+2"}, {"right", "Ctrl+3"}, {"iso", "Ctrl+4"}, {"bottom", "Ctrl+5"}, {"back", "Ctrl+6"}, {"left", "Ctrl+7"}})
    addAction("view." + name, tr("View: %1").arg(name), "home", QKeySequence(key), [this, n = name] { m_viewport->standardView(n); });
  QAction* ortho = addAction("view.ortho", tr("Orthographic"), "ortho", QKeySequence("O"), [this] {}, true);
  ortho->setChecked(true);
  connect(ortho, &QAction::toggled, this, [this](bool on) { m_viewport->setOrthographic(on); updateChips(); });
  QAction* shaded = addAction("view.shaded", tr("Shaded"), "shaded", QKeySequence("5"), [this] {}, true);
  QAction* edges = addAction("view.edges", tr("Shaded + edges"), "shadedEdges", QKeySequence("6"), [this] {}, true);
  QAction* wire = addAction("view.wire", tr("Wireframe"), "wireframe", QKeySequence("7"), [this] {}, true);
  auto* styleGroup = new QActionGroup(this);
  for (QAction* a : {shaded, edges, wire}) styleGroup->addAction(a);
  edges->setChecked(true);
  connect(styleGroup, &QActionGroup::triggered, this, [this, shaded, wire](QAction* a) {
    m_viewport->setStyle(a == shaded ? Viewport::Style::Shaded : a == wire ? Viewport::Style::Wireframe : Viewport::Style::ShadedEdges);
    updateChips();
  });
  QAction* grid = addAction("view.grid", tr("Grid"), "grid", QKeySequence("G"), [this] {}, true);
  connect(grid, &QAction::toggled, this, [this](bool on) { m_viewport->setGrid(on); });
  QAction* section = addAction("view.section", tr("Section"), "section", QKeySequence("X"), [this] {}, true);
  connect(section, &QAction::toggled, this, [this](bool on) {
    m_section->setEnabled(on);
    if (on) m_inspector->setCurrentWidget(m_section);
  });
  addAction("view.flip", tr("Flip section"), "flip", QKeySequence("Shift+X"), [this] { m_section->flip(); });
  addAction("view.isolate", tr("Isolate"), "isolate", QKeySequence("I"), [this] { m_viewport->isolate(currentNodeIds()); });
  addAction("view.unisolate", tr("Exit isolate"), "showAll", QKeySequence("Shift+I"), [this] { m_viewport->isolate({}); });
  addAction("view.saveview", tr("Save view…"), "home", QKeySequence(), [this] { saveNamedView(); });
  m_darkAction = addAction("view.dark", tr("&Dark theme"), "", QKeySequence(), [this] {}, true);
  // Panel toggles: always enabled, so a closed dock can be reopened even with no document.
  addAction("panel.browser", tr("Browser"), "browse", QKeySequence("Ctrl+1"), [this] {}, true);
  addAction("panel.inspector", tr("Properties"), "doc", QKeySequence("Ctrl+2"), [this] {}, true);
  addAction("panel.timeline", tr("Timeline"), "commit", QKeySequence("Ctrl+3"), [this] {}, true);
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
    QAction* a = addAction("select." + name.toLower(), name, icon, QKeySequence(key), [this, ff = f, n = name] {
      m_viewport->setSelectionFilter(ff);
      for (QAction* o : m_actions) if (o->objectName().startsWith("select.")) o->setChecked(o->objectName() == "select." + n.toLower());
    }, true);
    if (f == Viewport::SelFilter::Body) a->setChecked(true);
  }

  // Inspect
  addAction("inspect.distance", tr("Distance"), "distance", QKeySequence("D"), [this] { measure("distance"); });
  addAction("inspect.angle", tr("Angle"), "angle", QKeySequence("A"), [this] { measure("angle"); });
  addAction("inspect.radius", tr("Radius"), "radius", QKeySequence("R"), [this] { measure("radius"); });
  addAction("inspect.bbox", tr("Bounding box"), "bbox", QKeySequence("B"), [this] { measure("bbox"); });
  m_pinAction = addAction("inspect.pin", tr("Pin"), "pin", QKeySequence("P"), [this] { pinMeasurement(); });
  m_pinAction->setEnabled(false);
  addAction("inspect.clear", tr("Clear measurement"), "", QKeySequence("Esc"), [this] { clearMeasurement(); });
  addAction("inspect.properties", tr("Properties"), "doc", QKeySequence("Ctrl+P"), [this] { showProperties(m_viewport->selection()); m_inspector->setCurrentWidget(m_props); });

  // Annotate / edit
  addAction("annotate.add", tr("Note"), "annotate", QKeySequence("N"), [this] { addAnnotation(); });
  addAction("annotate.resolve", tr("Resolve note"), "check", QKeySequence("Ctrl+Return"), [this] { resolveCurrentAnnotation(); });
  addAction("edit.undo", tr("&Undo"), "rollLeft", QKeySequence::Undo, [this] { m_doc->undo(); });
  addAction("edit.redo", tr("&Redo"), "rollRight", QKeySequence::Redo, [this] { m_doc->redo(); });
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
  addAction("edit.filter", tr("Filter objects"), "search", QKeySequence("Ctrl+F"), [this] { m_browser->focusFilter(); });
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
  setWindowIcon(icons::icon("body", theme::current().sel));
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
  add(edit, {"edit.undo", "edit.redo", "-", "edit.rename", "edit.hide", "edit.showall", "edit.filter", "edit.selectparent", "-", "annotate.add", "annotate.resolve", "-", "edit.delete", "edit.restore", "edit.selecttouched", "-", "select.bodies", "select.faces", "select.edges", "select.vertices"});
  QMenu* view = menuBar()->addMenu(tr("&View"));
  add(view, {"view.fit", "view.fitall", "view.home", "view.rollleft", "view.rollright", "-", "view.top", "view.front", "view.right", "view.iso", "view.bottom", "view.back", "view.left", "-", "view.ortho", "view.shaded", "view.edges", "view.wire", "view.grid", "-", "view.section", "view.flip", "view.isolate", "view.unisolate", "-", "view.saveview"});
  m_viewsMenu = view->addMenu(tr("Named views"));
  view->addSeparator();
  QMenu* nav = view->addMenu(tr("Navigation preset"));
  add(nav, {"nav.fusion", "nav.solidworks", "nav.onshape", "nav.blender"});
  add(view, {"view.dark", "-", "panel.browser", "panel.inspector", "panel.timeline", "panel.reset"});
  QMenu* inspect = menuBar()->addMenu(tr("&Inspect"));
  add(inspect, {"inspect.distance", "inspect.angle", "inspect.radius", "inspect.bbox", "inspect.pin", "inspect.clear", "-", "inspect.properties"});
  QMenu* tools = menuBar()->addMenu(tr("&Tools"));
  add(tools, {"tools.commands", "tools.shortcuts", "tools.cache"});
  QMenu* help = menuBar()->addMenu(tr("&Help"));
  add(help, {"help.about"});
  rebuildRecentMenu();
}

void MainWindow::buildRibbon() {
  m_ribbon = new RibbonBar(this);
  auto acts = [&](std::initializer_list<const char*> ids) {
    QList<QAction*> out;
    for (const char* id : ids) if (QAction* a = action(id)) out << a;
    return out;
  };
  m_ribbon->addTab(tr("View"), {acts({"view.fit", "view.home", "view.ortho"}), acts({"view.shaded", "view.edges", "view.wire", "view.grid"}), acts({"view.section", "view.isolate", "view.unisolate"})});
  m_ribbon->addTab(tr("Inspect"), {acts({"inspect.distance", "inspect.angle", "inspect.radius", "inspect.bbox"}), acts({"inspect.pin", "inspect.properties"})});
  m_ribbon->addTab(tr("Annotate"), {acts({"annotate.add", "annotate.resolve"}), acts({"edit.rename", "edit.hide", "edit.showall", "view.saveview"})});
  m_ribbon->addTab(tr("Export"), {acts({"file.export", "file.screenshot"}), acts({"file.import", "file.save"})});
  m_ribbon->setSelectFilters(acts({"select.bodies", "select.faces", "select.edges", "select.vertices"}), {"1", "2", "3", "4"});
  m_ribbon->setSearchAction(action("tools.commands"));
  QAction* settingsAction = addAction("tools.settings", tr("Settings"), "settings", QKeySequence(), [] {});
  auto* settings = new QMenu(this);
  settings->addAction(action("view.dark"));
  QMenu* navMenu = settings->addMenu(tr("Navigation preset"));
  for (QAction* a : m_actions) if (a->objectName().startsWith("nav.")) navMenu->addAction(a);
  settings->addSeparator();
  settings->addAction(action("panel.browser"));
  settings->addAction(action("panel.inspector"));
  settings->addAction(action("panel.timeline"));
  settings->addAction(action("panel.reset"));
  settings->addSeparator();
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
  m_stack->addWidget(m_empty);
  m_stack->addWidget(m_viewport);
  setCentralWidget(m_stack);

  // Native child widgets float above the OpenGL surface: top-left chips, bottom-right measurement card.
  m_chips = new ViewportChips(m_viewport);
  m_chips->setAttribute(Qt::WA_NativeWindow);
  m_measureCard = new MeasureCard(m_viewport);
  m_measureCard->setAttribute(Qt::WA_NativeWindow);
  m_measureCard->hide();
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
  // Right dock takes the full height; the timeline strip runs under the browser and the viewport.
  setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
  setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
  setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
  setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);

  m_browser = new BrowserPanel(m_doc, this);
  auto* left = m_browserDock = new QDockWidget(tr("Browser"), this);
  left->setObjectName("dock.browser");
  left->setTitleBarWidget(new DockHeader(tr("Browser"), left));
  left->setWidget(m_browser);
  addDockWidget(Qt::LeftDockWidgetArea, left);

  m_inspector = new QTabWidget(this);
  m_inspector->setObjectName("inspector");
  m_inspector->setTabPosition(QTabWidget::South);
  m_inspector->setDocumentMode(true);
  m_props = new PropertiesPanel(m_inspector);
  m_annotations = new AnnotationsPanel(m_doc, m_inspector);
  m_section = new SectionPanel(m_doc, m_inspector);
  m_inspector->addTab(m_props, tr("Properties"));
  m_inspector->addTab(m_annotations, tr("Annotations"));
  m_inspector->addTab(m_section, tr("Section"));
  auto* right = m_inspectorDock = new QDockWidget(tr("Properties"), this);
  right->setObjectName("dock.inspector");
  auto* rightHeader = new DockHeader(tr("Properties"), right);
  right->setTitleBarWidget(rightHeader);
  right->setWidget(m_inspector);
  addDockWidget(Qt::RightDockWidgetArea, right);
  connect(m_inspector, &QTabWidget::currentChanged, this, [this, rightHeader, right](int i) {
    rightHeader->setTitle(m_inspector->tabText(i));
    right->setWindowTitle(m_inspector->tabText(i));
  });

  m_timeline = new TimelineWidget(m_doc, this);
  auto* bottom = m_timelineDock = new QDockWidget(tr("Timeline"), this);
  bottom->setObjectName("dock.timeline");
  bottom->setTitleBarWidget(new QWidget(bottom));  // the strip is its own header
  bottom->setFeatures(QDockWidget::NoDockWidgetFeatures);
  bottom->setWidget(m_timeline);
  bottom->setFixedHeight(48);
  addDockWidget(Qt::BottomDockWidgetArea, bottom);

  resizeDocks({left, right}, {352, 384}, Qt::Horizontal);
  m_props->clear();

  // Bind the panel actions to the docks' own toggle actions (both directions).
  bindPanel(action("panel.browser"), left);
  bindPanel(action("panel.inspector"), right);
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

void MainWindow::resetLayout() {
  for (QDockWidget* d : {m_browserDock, m_inspectorDock, m_timelineDock}) {
    if (!d) continue;
    d->setFloating(false);
    d->show();
  }
  addDockWidget(Qt::LeftDockWidgetArea, m_browserDock);
  addDockWidget(Qt::RightDockWidgetArea, m_inspectorDock);
  addDockWidget(Qt::BottomDockWidgetArea, m_timelineDock);
  setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
  setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
  setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
  setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
  resizeDocks({m_browserDock, m_inspectorDock}, {352, 384}, Qt::Horizontal);
}

void MainWindow::buildStatusBar() {
  const Tokens& t = theme::current();
  m_statusPath = new QLabel(this);
  m_statusPath->setFont(theme::mono(12));
  m_statusGitIcon = new QLabel(this);
  m_statusGitIcon->setPixmap(icons::pixmap("git", t.fg2, 14, devicePixelRatioF()));
  m_statusGit = new QLabel(this);
  m_statusGit->setTextFormat(Qt::RichText);
  m_statusHover = new QLabel(this);
  m_statusHover->setAlignment(Qt::AlignCenter);
  m_statusHover->setObjectName("tertiary");
  m_statusSel = new QLabel(this);
  m_statusUnits = new QLabel("mm", this);
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
  statusBar()->addPermanentWidget(m_statusSel);
  statusBar()->addPermanentWidget(m_statusUnits);
  statusBar()->setSizeGripEnabled(false);
  connect(m_jobs, &JobRunner::stripShown, this, [this](bool shown) { m_statusHover->setVisible(!shown); });  // free room for the bars
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
  m_browser->setViewerMode(m_doc->browse);
  updateUndoActions();
  m_inspector->setTabVisible(m_inspector->indexOf(m_annotations), !m_doc->browse);
  if (m_doc->browse && m_timelineDock->isVisible()) { m_timelineDock->hide(); m_timelineHiddenByViewer = true; }
  else if (!m_doc->browse && m_timelineHiddenByViewer) { m_timelineDock->show(); m_timelineHiddenByViewer = false; }
}

void MainWindow::updateTitle() {
  setWindowTitle(m_doc->title());
  QString path = m_doc->hasDocument ? (m_doc->browse ? tr("viewing: ") + m_settings.value("ui/lastBrowse").toString() : (m_doc->path().isEmpty() ? tr("unsaved document") : m_doc->path())) : tr("No document");
  if (!m_doc->scene.unresolved.empty()) path += QString::fromUtf8("   ·   %1 unresolved").arg(m_doc->scene.unresolved.size());
  m_statusPath->setText(path);
  if (!m_doc->hasDocument) m_statusHover->setText(QString::fromUtf8("File › Open a .step or .opad file, or drop one here"));
  else if (m_statusHover->text().startsWith("File ")) m_statusHover->clear();
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
  m_chips->move(0, 0);
  m_chips->raise();
  m_homeBtn->adjustSize();
  m_homeBtn->move(m_viewport->width() - 200, 12);  // left of the cube's axes, level with its top
  m_homeBtn->raise();
  m_rollLeft->move(m_viewport->width() - 200, 150);  // lower-left and lower-right of the cube
  m_rollLeft->raise();
  m_rollRight->move(m_viewport->width() - 40, 150);
  m_rollRight->raise();
  if (m_loadShade->isVisible()) {
    QRect area = m_stack->geometry();  // the workspace: central area plus the docked panels
    for (QDockWidget* d : {m_browserDock, m_inspectorDock, m_timelineDock})
      if (d && d->isVisible() && !d->isFloating()) area |= d->geometry();
    m_loadShade->place(QRect(mapToGlobal(area.topLeft()), area.size()), m_viewport->mapToGlobal(m_viewport->rect().center()));
  }
  if (trace::enabled()) trace::log(QStringLiteral("viewport at %1,%2 size %3x%4").arg(m_viewport->mapToGlobal(QPoint(0, 0)).x()).arg(m_viewport->mapToGlobal(QPoint(0, 0)).y()).arg(m_viewport->width()).arg(m_viewport->height()));
  m_measureCard->move(m_viewport->width() - m_measureCard->width() - 16, m_viewport->height() - m_measureCard->height() - 16);
  m_measureCard->raise();
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
  for (QWidget* w : {static_cast<QWidget*>(m_browser), static_cast<QWidget*>(m_inspector), static_cast<QWidget*>(m_timeline)}) w->setEnabled(!on);
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
  if (m_syncing) return;
  m_syncing = true;
  auto refs = m_viewport->selection();
  std::vector<std::string> ids;
  std::set<std::string> seen;
  for (const auto& r : refs)
    if (seen.insert(r.body).second) ids.push_back(r.body);
  m_browser->setSelectedIds(ids);
  if (m_section && m_section->picking() && !refs.empty() && refs.front().kind == opad::Ref::Kind::Face) sectionFromFace(refs.front());
  showProperties(refs);
  if (refs.empty()) m_statusSel->clear();
  else m_statusSel->setText(QString::fromUtf8("%1 selected · %2").arg(refs.size()).arg(opad::Ref::kind_name(refs.front().kind)));
  scheduleSelectionSync();
  m_syncing = false;
}

void MainWindow::onBrowserSelection(const std::vector<std::string>& ids) {
  if (m_syncing) return;
  m_syncing = true;
  std::vector<opad::Ref> refs;
  for (const auto& id : ids) { opad::Ref r; r.body = id; refs.push_back(r); }
  showProperties(refs);  // O(1): only the first ref is inspected and geometry walks are deferred to a job
  m_statusSel->setText(ids.empty() ? QString() : QString::fromUtf8("%1 selected · body").arg(ids.size()));
  m_syncing = false;
  m_viewport->selectNodes(ids);  // sliced; selectionApplied() writes selection.json when it settles
}

void MainWindow::showProperties(const std::vector<opad::Ref>& refs) {
  if (refs.empty()) {
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
        if (it != m_doc->scene.instance_count.end() && it->second > 1) subtitle += QString::fromUtf8(" · %1 instances").arg(it->second);
      }
      id = QString::fromStdString(r.body.substr(0, 8));
    } else {
      QString kind = QString::fromStdString(opad::Ref::kind_name(r.kind));
      QString geo = QString::fromStdString(j.value("surface", j.value("curve", std::string())));
      title = QString::fromUtf8("%1%2%3").arg(kind.left(1).toUpper() + kind.mid(1), geo.isEmpty() ? QString() : QString::fromUtf8(" · "), geo);
      subtitle = QString::fromUtf8("%1 › %2 %3").arg(m_doc->nodeName(r.body), kind).arg(r.index);
      id = QString::fromStdString(r.body.substr(0, 8));
    }
    if (refs.size() > 1) subtitle += tr("  (+%1 more)").arg(refs.size() - 1);
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
  QMenu menu(this);
  auto add = [&](const char* id) { if (QAction* a = action(id); a && (!m_doc->browse || a->isEnabled())) menu.addAction(a); };  // viewer mode: editing entries are not offered
  if (!ids.empty()) {
    menu.addSection(ids.size() == 1 ? m_doc->nodeName(ids.front()) : tr("%1 objects").arg(ids.size()));
    QAction* fit = menu.addAction(icons::themed("fit", 16), tr("Fit to"));
    connect(fit, &QAction::triggered, this, [this, ids] { m_viewport->fitNodes(ids); });
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

void MainWindow::timelineMenu(const std::string& opId, const QPoint& globalPos) {
  bool deleted = std::find(m_doc->scene.deleted_ops.begin(), m_doc->scene.deleted_ops.end(), opId) != m_doc->scene.deleted_ops.end();
  QMenu menu(this);
  menu.setFixedWidth(232);
  QAction* del = menu.addAction(icons::themed("delete", 16), tr("Delete (tombstone)\tDel"));
  del->setEnabled(!deleted);
  QAction* restore = menu.addAction(icons::themed("restore", 16), tr("Restore\tShift+Del"));
  restore->setEnabled(deleted);
  QAction* sel = menu.addAction(icons::themed("isolate", 16), tr("Select what it touches\tT"));
  menu.addSeparator();
  QAction* copy = menu.addAction(icons::themed("commit", 16), tr("Copy op id\tCtrl+C"));
  QAction* log = menu.addAction(icons::themed("git", 16), tr("Show in git log"));
  QAction* chosen = menu.exec(globalPos);
  if (chosen == del) deleteOp(opId);
  else if (chosen == restore) restoreOp(opId);
  else if (chosen == sel) selectOpTargets(opId);
  else if (chosen == copy) QApplication::clipboard()->setText(QString::fromStdString(opId));
  else if (chosen == log) {
    if (m_doc->doc.path.empty()) throw opad::Error("Save the document in a git repository first.");
    QFileInfo fi(m_doc->path());
    QProcess git;
    git.setWorkingDirectory(fi.absolutePath());
    git.start("git", {"log", "--format=%h %ad %an  %s", "--date=short", "-S", QString::fromStdString(opId), "--", fi.fileName()});
    git.waitForFinished(3000);
    QString out = QString::fromUtf8(git.readAllStandardOutput()).trimmed();
    QMessageBox::information(this, tr("git log for op %1").arg(QString::fromStdString(opId.substr(0, 8))), out.isEmpty() ? tr("Not committed yet.") : out);
  }
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

void MainWindow::measure(const QString& kind) {
  auto refs = m_viewport->selection();
  std::vector<std::string> strs;
  QStringList targets;
  for (const auto& r : refs) {
    strs.push_back(r.str());
    QString t = m_doc->nodeName(r.body);
    if (r.kind != opad::Ref::Kind::Body) t += QString::fromUtf8(" › %1 %2").arg(opad::Ref::kind_name(r.kind)).arg(r.index);
    targets << t;
  }
  if (strs.empty())
    for (const auto& id : m_browser->selectedIds()) { strs.push_back(id); targets << m_doc->nodeName(id); }
  if (strs.empty()) throw opad::Error("Pick the faces, edges or bodies to measure first (Select filter 1–4).");
  opad::json args{{"kind", kind.toStdString()}, {"refs", strs}};
  m_lastMeasure = opad::commands::run("measure", args, &m_doc->doc);
  m_lastMeasureTargets = targets;
  m_measureCard->setResult(m_lastMeasure, targets);
  m_measureCard->show();
  positionOverlays();
  m_pinAction->setEnabled(!m_doc->browse);
  if (m_lastMeasure.contains("point_a") && m_lastMeasure.contains("point_b")) {
    const auto& a = m_lastMeasure["point_a"];
    const auto& b = m_lastMeasure["point_b"];
    m_viewport->showDimension({a[0].get<double>(), a[1].get<double>(), a[2].get<double>()}, {b[0].get<double>(), b[1].get<double>(), b[2].get<double>()},
                              QString("%1 mm").arg(m_lastMeasure["value"].get<double>(), 0, 'f', 3));
  } else {
    m_viewport->clearDimension();
  }
  statusBar()->showMessage(tr("%1: pick more references, P pins, Esc clears").arg(kind), 8000);
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
  statusBar()->showMessage(tr("Measurement pinned to the document"), 4000);
}

void MainWindow::clearMeasurement() {
  m_lastMeasure = opad::json();
  m_measureCard->hide();
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
  bool ok = false;
  QString where = m_doc->nodeName(anchor.body);
  if (anchor.kind != opad::Ref::Kind::Body) where += QString::fromUtf8(" › %1 %2").arg(opad::Ref::kind_name(anchor.kind)).arg(anchor.index);
  QString text = QInputDialog::getMultiLineText(this, tr("Note on %1").arg(where), tr("Note (Ctrl+Enter resolves it later):"), QString(), &ok);
  if (!ok || text.trimmed().isEmpty()) return;
  opad::json r = m_doc->run("annotate", opad::json{{"anchor", anchor.str()}, {"text", text.toStdString()}});
  if (r.contains("id")) m_timeline->setCurrentOp(r["id"].get<std::string>());
  m_inspector->setCurrentWidget(m_annotations);
}

void MainWindow::resolveCurrentAnnotation() {
  std::string id = m_annotations->currentOpId();
  if (id.empty()) id = m_timeline->currentOp();
  const opad::Op* op = id.empty() ? nullptr : m_doc->doc.find_op(id);
  if (!op || op->type != "annotation") throw opad::Error("Select a note in the Annotations tab or on the timeline first.");
  deleteOp(id);
}

void MainWindow::deleteOp(const std::string& opId) {
  opad::json r = m_doc->run("delete", opad::json{{"target", opId}});
  if (r.contains("id")) m_timeline->setCurrentOp(opId);
}

void MainWindow::restoreOp(const std::string& opId) {
  // Restoring = tombstoning the delete op that targets it.
  for (const auto& op : m_doc->doc.ops)
    if (op.type == "delete" && op.data.value("target", "") == opId && !m_doc->doc.is_deleted(op.id)) {
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
  if (op->type == "import") {
    std::function<void(const opad::json&)> walk = [&](const opad::json& nodes) {
      for (const auto& n : nodes) { ids.push_back(n.value("id", "")); if (n.contains("children")) walk(n["children"]); }
    };
    walk(d.value("nodes", opad::json::array()));
  }
  ids.erase(std::remove_if(ids.begin(), ids.end(), [&](const std::string& id) { return !m_doc->node(id); }), ids.end());
  m_browser->setSelectedIds(ids);
  onBrowserSelection(ids);
  m_props->showEntity(m_timeline->describe(*op), QString::fromUtf8("%1 · %2").arg(QString::fromStdString(d.value("by", "")), QString::fromStdString(d.value("ts", "")).left(16).replace('T', ' ')),
                      QString::fromStdString(opId.substr(0, 8)), d);
}

// ---------------------------------------------------------------- export (F14/F15)
void MainWindow::exportDialog() {
  if (!m_doc->hasDocument) throw opad::Error("Nothing to export.");
  auto ids = currentNodeIds();
  int selBodies = 0;
  for (const auto& id : ids) selBodies += static_cast<int>(m_doc->scene.bodies_under(id).size());
  int allBodies = static_cast<int>(m_doc->scene.all_bodies().size());

  QDialog dlg(this);
  dlg.setWindowTitle(tr("Export"));
  dlg.setFixedWidth(560);
  auto* v = new QVBoxLayout(&dlg);
  v->setContentsMargins(16, 16, 16, 16);
  v->setSpacing(12);
  auto header = [&](const QString& t) { auto* l = new QLabel(t, &dlg); l->setObjectName("sectionHeader"); v->addWidget(l); };
  header(tr("FORMAT"));
  auto* grid = new QGridLayout();
  auto* group = new QButtonGroup(&dlg);
  struct Fmt { QString label, format, schema; };
  QList<Fmt> fmts = {{"STEP AP214", "step", "AP214"}, {"STEP AP242", "step", "AP242"}, {"OBJ (+MTL)", "obj", ""}, {"STL", "stl", ""}, {"GLB", "glb", ""}};
  for (const auto& f : opad::commands::exporter_formats())
    if (f != "step" && f != "obj" && f != "stl" && f != "glb") fmts << Fmt{QString::fromStdString(f).toUpper() + tr(" (plugin)"), QString::fromStdString(f), ""};
  int i = 0;
  for (const auto& f : fmts) {
    auto* r = new QRadioButton(f.label, &dlg);
    r->setProperty("format", f.format);
    r->setProperty("schema", f.schema);
    group->addButton(r, i);
    grid->addWidget(r, i / 2, i % 2);
    if (i == 1) r->setChecked(true);
    ++i;
  }
  v->addLayout(grid);
  header(tr("OBJECTS"));
  auto* scopeRow = new QHBoxLayout();
  auto* scopeSel = new QRadioButton(QString::fromUtf8("Selection · %1 %2").arg(selBodies).arg(selBodies == 1 ? tr("body") : tr("bodies")), &dlg);
  auto* scopeAll = new QRadioButton(QString::fromUtf8("Whole document · %1 bodies").arg(allBodies), &dlg);
  scopeSel->setEnabled(selBodies > 0);
  (selBodies > 0 ? scopeSel : scopeAll)->setChecked(true);
  scopeRow->addWidget(scopeSel);
  scopeRow->addWidget(scopeAll);
  scopeRow->addStretch();
  v->addLayout(scopeRow);
  auto* form = new QFormLayout();
  auto* tol = new QDoubleSpinBox(&dlg);
  tol->setRange(0.001, 10);
  tol->setDecimals(3);
  tol->setValue(0.010);
  tol->setSuffix(" mm");
  tol->setFont(theme::mono(12));
  form->addRow(tr("Tolerance"), tol);
  v->addLayout(form);
  header(tr("OPTIONS"));
  auto* perBody = new QCheckBox(tr("One file per body (STL)"), &dlg);
  perBody->setChecked(true);
  auto* ascii = new QCheckBox(tr("ASCII STL"), &dlg);
  auto* mtl = new QCheckBox(tr("Write material library (OBJ)"), &dlg);
  mtl->setChecked(true);
  v->addWidget(perBody);
  v->addWidget(ascii);
  v->addWidget(mtl);
  header(tr("TARGET"));
  auto* pathRow = new QHBoxLayout();
  auto* path = new QLineEdit(&dlg);
  path->setObjectName("mono");
  path->setFont(theme::mono(12));
  QString stem = m_doc->doc.path.empty() ? "export" : QString::fromStdString(m_doc->doc.path.stem().string());
  path->setText(QDir(m_settings.value("ui/lastDir", QDir::homePath()).toString()).filePath(stem + ".step"));
  auto* browse = new QPushButton(tr("Browse…"), &dlg);
  pathRow->addWidget(path, 1);
  pathRow->addWidget(browse);
  v->addLayout(pathRow);
  auto* footer = new QHBoxLayout();
  auto* summary = new QLabel(&dlg);
  summary->setObjectName("secondary");
  footer->addWidget(summary, 1);
  auto* cancel = new QPushButton(tr("Cancel   Esc"), &dlg);
  auto* ok = new QPushButton(tr("Export   Enter"), &dlg);
  ok->setObjectName("primary");
  ok->setDefault(true);
  footer->addWidget(cancel);
  footer->addWidget(ok);
  v->addLayout(footer);
  auto refresh = [&] {
    auto* b = group->checkedButton();
    QString fmt = b ? b->property("format").toString() : "step";
    QFileInfo fi(path->text());
    QString ext = fmt == "step" ? "step" : fmt;
    if (fi.suffix().toLower() != ext && !(fmt == "step" && fi.suffix().toLower() == "stp")) path->setText(fi.dir().filePath(fi.completeBaseName() + "." + ext));
    int n = scopeSel->isChecked() ? selBodies : allBodies;
    summary->setText(QString::fromUtf8("%1 · %2 bodies · %3 mm").arg(b ? b->text() : fmt).arg(n).arg(tol->value(), 0, 'f', 3));
    perBody->setEnabled(fmt == "stl");
    ascii->setEnabled(fmt == "stl");
    mtl->setEnabled(fmt == "obj");
  };
  connect(group, &QButtonGroup::idClicked, &dlg, [&](int) { refresh(); });
  connect(scopeSel, &QRadioButton::toggled, &dlg, [&](bool) { refresh(); });
  connect(tol, &QDoubleSpinBox::valueChanged, &dlg, [&](double) { refresh(); });
  connect(browse, &QPushButton::clicked, &dlg, [&] {
    QString p = QFileDialog::getSaveFileName(&dlg, tr("Export to"), path->text());
    if (!p.isEmpty()) path->setText(p);
  });
  connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
  connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
  refresh();
  if (dlg.exec() != QDialog::Accepted) return;
  auto* b = group->checkedButton();
  QString fmt = b->property("format").toString();
  QString out = path->text();
  m_settings.setValue("ui/lastDir", QFileInfo(out).absolutePath());
  opad::json args{{"format", fmt.toStdString()}, {"out", out.toStdString()}, {"tolerance", tol->value()}, {"ascii", ascii->isChecked()}, {"per_body", perBody->isChecked()}, {"mtl", mtl->isChecked()}};
  if (!b->property("schema").toString().isEmpty()) args["schema"] = b->property("schema").toString().toStdString();
  if (scopeSel->isChecked()) args["select"] = ids;
  opad::json r = opad::commands::run("export", args, &m_doc->doc);
  statusBar()->showMessage(tr("Exported %1 bodies to %2").arg(r.value("bodies", 0)).arg(out), 8000);
}

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
  beginLoad([this, path] { addRecent(path); m_viewport->fitWhenReady(); });
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
  m_benchSelect = false;
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
      m_section->beginPick();  // then a face pick must set the section plane (logged as "section from face")
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
    if (ext == "step" || ext == "stp" || ext == "opad") { openPath(p); return; }
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

void MainWindow::closeEvent(QCloseEvent* e) {
  if (!maybeSave()) {
    e->ignore();
    return;
  }
  m_settings.setValue("ui/geometry", saveGeometry());
  m_settings.setValue("ui/state", saveState());
  m_settings.setValue("ui/layoutVersion", 2);
  e->accept();
}
