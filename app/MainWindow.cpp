#include "MainWindow.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSlider>
#include <QStatusBar>
#include <QStyleFactory>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <fstream>

#include "opad/inspect.hpp"

MainWindow::MainWindow() : m_doc(new AppDocument(this)) {
  setWindowTitle("OPAD");
  resize(1400, 900);
  m_viewport = new Viewport(m_doc, this);
  setCentralWidget(m_viewport);
  buildActions();
  buildMenus();
  buildToolbar();
  buildDocks();

  m_statusPath = new QLabel(this);
  m_statusGit = new QLabel(this);
  m_statusSel = new QLabel(this);
  m_statusHover = new QLabel(this);
  statusBar()->addWidget(m_statusPath, 2);
  statusBar()->addWidget(m_statusHover, 1);
  statusBar()->addPermanentWidget(m_statusSel);
  statusBar()->addPermanentWidget(m_statusGit);

  connect(m_doc, &AppDocument::changed, this, [this] { updateTitle(); rebuildViewsMenu(); });
  connect(m_doc, &AppDocument::pathChanged, this, [this] { updateTitle(); refreshGit(); });
  connect(m_doc, &AppDocument::message, this, [this](const QString& t) { statusBar()->showMessage(t, 6000); });
  connect(m_viewport, &Viewport::selectionChanged, this, &MainWindow::onViewportSelection);
  connect(m_viewport, &Viewport::hoverChanged, m_statusHover, &QLabel::setText);
  connect(m_viewport, &Viewport::contextMenuRequested, this, [this](const QPoint& p) { showContextMenu(p, currentNodeIds()); });
  connect(m_viewport, &Viewport::meshingProgress, this, [this](int remaining) {
    if (remaining > 0) statusBar()->showMessage(tr("Tessellating %1 bodies...").arg(remaining));
    else statusBar()->clearMessage();
  });
  connect(m_browser, &BrowserPanel::selectionChanged, this, &MainWindow::onBrowserSelection);
  connect(m_browser, &BrowserPanel::contextMenuRequested, this, [this](const QPoint& p, const std::vector<std::string>& ids) { showContextMenu(p, ids); });
  connect(m_browser, &BrowserPanel::fitRequested, m_viewport, &Viewport::fitNodes);
  connect(m_annotations, &AnnotationsPanel::addRequested, this, &MainWindow::addAnnotation);
  connect(m_annotations, &AnnotationsPanel::resolveRequested, this, &MainWindow::deleteOp);
  connect(m_annotations, &AnnotationsPanel::selectNode, this, [this](const std::string& id) { onBrowserSelection({id}); m_browser->setSelectedIds({id}); });
  connect(m_timeline, &TimelineWidget::opClicked, this, &MainWindow::selectOpTargets);
  connect(m_timeline, &TimelineWidget::deleteRequested, this, [this](const std::string& id) {
    QMenu menu(this);
    bool deleted = std::find(m_doc->scene.deleted_ops.begin(), m_doc->scene.deleted_ops.end(), id) != m_doc->scene.deleted_ops.end();
    QAction* del = menu.addAction(deleted ? tr("Restore (delete the tombstone)") : tr("Delete (tombstone)"));
    QAction* sel = menu.addAction(tr("Select what it touches"));
    QAction* chosen = menu.exec(QCursor::pos());
    if (chosen == del) deleteOp(id);
    else if (chosen == sel) selectOpTargets(id);
  });

  m_gitTimer.setInterval(5000);
  connect(&m_gitTimer, &QTimer::timeout, this, &MainWindow::refreshGit);
  m_gitTimer.start();

  applyTheme(m_settings.value("ui/dark", true).toBool());
  restoreGeometry(m_settings.value("ui/geometry").toByteArray());
  restoreState(m_settings.value("ui/state").toByteArray());
  updateTitle();
}

// ---------------------------------------------------------------- actions
QAction* MainWindow::addAction(const QString& id, const QString& text, const QKeySequence& shortcut, std::function<void()> fn, bool checkable) {
  auto* a = new QAction(text, this);
  a->setObjectName(id);
  QString saved = m_settings.value("shortcuts/" + id).toString();
  a->setShortcut(saved.isEmpty() ? shortcut : QKeySequence(saved));
  a->setCheckable(checkable);
  a->setShortcutContext(Qt::WindowShortcut);
  connect(a, &QAction::triggered, this, [this, fn] { guarded(fn); });
  m_actions << a;
  QMainWindow::addAction(a);
  return a;
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
  addAction("file.new", tr("&New document"), QKeySequence::New, [this] { if (maybeSave()) m_doc->newDocument(); });
  addAction("file.open", tr("&Open... (browse STEP or open .opad)"), QKeySequence::Open, [this] {
    if (!maybeSave()) return;
    QString p = QFileDialog::getOpenFileName(this, tr("Open"), m_settings.value("ui/lastDir").toString(), tr("OPAD or STEP (*.opad *.step *.stp);;OPAD document (*.opad);;STEP (*.step *.stp)"));
    if (!p.isEmpty()) openPath(p);
  });
  addAction("file.import", tr("&Import STEP into document..."), QKeySequence("Ctrl+I"), [this] {
    QString p = QFileDialog::getOpenFileName(this, tr("Import STEP"), m_settings.value("ui/lastDir").toString(), tr("STEP (*.step *.stp)"));
    if (p.isEmpty()) return;
    m_settings.setValue("ui/lastDir", QFileInfo(p).absolutePath());
    auto ids = currentNodeIds();
    QString parent;
    if (ids.size() == 1 && m_doc->node(ids[0]) && m_doc->node(ids[0])->kind == opad::Node::Kind::Component) {
      if (QMessageBox::question(this, tr("Import"), tr("Import under the selected component \"%1\"?").arg(m_doc->nodeName(ids[0]))) == QMessageBox::Yes) parent = QString::fromStdString(ids[0]);
    }
    m_doc->importStep(p, parent);
    m_viewport->fitAll();
  });
  addAction("file.save", tr("&Save"), QKeySequence::Save, [this] {
    if (m_doc->browse) throw opad::Error("Browse mode shows a STEP file without a document. Use Import to create one.");
    if (m_doc->doc.path.empty()) m_actions.at(4)->trigger();
    else m_doc->save();
  });
  addAction("file.saveas", tr("Save &As..."), QKeySequence::SaveAs, [this] {
    if (m_doc->browse) throw opad::Error("Browse mode shows a STEP file without a document. Use Import to create one.");
    QString p = QFileDialog::getSaveFileName(this, tr("Save document"), m_settings.value("ui/lastDir").toString(), tr("OPAD document (*.opad)"));
    if (p.isEmpty()) return;
    if (!p.endsWith(".opad", Qt::CaseInsensitive)) p += ".opad";
    m_settings.setValue("ui/lastDir", QFileInfo(p).absolutePath());
    m_doc->saveAs(p);
  });
  addAction("file.export", tr("&Export..."), QKeySequence("Ctrl+E"), [this] { exportDialog(); });
  addAction("file.screenshot", tr("Save screens&hot..."), QKeySequence("Ctrl+Shift+P"), [this] { screenshot(); });
  addAction("file.quit", tr("&Quit"), QKeySequence::Quit, [this] { close(); });

  // View
  addAction("view.fit", tr("&Fit all"), QKeySequence("F"), [this] { m_viewport->fitAll(); });
  addAction("view.fitsel", tr("Fit &selection"), QKeySequence("Shift+F"), [this] { m_viewport->fitSelection(); });
  addAction("view.home", tr("&Home view"), QKeySequence("H"), [this] { m_viewport->home(); });
  for (const auto& [name, key] : std::vector<std::pair<QString, QString>>{{"top", "Ctrl+1"}, {"front", "Ctrl+2"}, {"right", "Ctrl+3"}, {"iso", "Ctrl+4"}, {"bottom", "Ctrl+5"}, {"back", "Ctrl+6"}, {"left", "Ctrl+7"}})
    addAction("view." + name, tr("View: %1").arg(name), QKeySequence(key), [this, n = name] { m_viewport->standardView(n); });
  QAction* ortho = addAction("view.ortho", tr("&Orthographic projection"), QKeySequence("O"), [this] {}, true);
  ortho->setChecked(true);
  connect(ortho, &QAction::toggled, this, [this](bool on) { m_viewport->setOrthographic(on); });
  QAction* shaded = addAction("view.shaded", tr("Display: Shaded"), QKeySequence("Alt+1"), [this] { m_viewport->setStyle(Viewport::Style::Shaded); });
  QAction* edges = addAction("view.edges", tr("Display: Shaded with edges"), QKeySequence("Alt+2"), [this] { m_viewport->setStyle(Viewport::Style::ShadedEdges); });
  QAction* wire = addAction("view.wire", tr("Display: Wireframe"), QKeySequence("Alt+3"), [this] { m_viewport->setStyle(Viewport::Style::Wireframe); });
  (void)shaded; (void)edges; (void)wire;
  QAction* grid = addAction("view.grid", tr("Ground &grid"), QKeySequence("G"), [this] {}, true);
  connect(grid, &QAction::toggled, this, [this](bool on) { m_viewport->setGrid(on); });
  QAction* shadows = addAction("view.shadows", tr("Shado&ws"), QKeySequence(), [this] {}, true);
  connect(shadows, &QAction::toggled, this, [this](bool on) { m_viewport->setShadows(on); });
  addAction("view.section", tr("&Section analysis..."), QKeySequence("Ctrl+Shift+S"), [this] { sectionDialog(); });
  addAction("view.isolate", tr("&Isolate selection"), QKeySequence("I"), [this] { m_viewport->isolate(currentNodeIds()); });
  addAction("view.unisolate", tr("Show &everything (un-isolate)"), QKeySequence("Shift+I"), [this] { m_viewport->isolate({}); });
  addAction("view.saveview", tr("Save named &view..."), QKeySequence(), [this] { saveNamedView(); });
  m_darkAction = addAction("view.dark", tr("&Dark theme"), QKeySequence(), [this] {}, true);
  connect(m_darkAction, &QAction::toggled, this, [this](bool on) { applyTheme(on); });
  for (const auto& [name, preset] : std::vector<std::pair<QString, Viewport::NavPreset>>{{"Fusion", Viewport::NavPreset::Fusion}, {"SolidWorks", Viewport::NavPreset::SolidWorks}, {"Onshape", Viewport::NavPreset::Onshape}, {"Blender", Viewport::NavPreset::Blender}}) {
    QAction* a = addAction("nav." + name.toLower(), tr("Navigation: %1").arg(name), QKeySequence(), [this, p = preset, n = name] {
      m_viewport->setNavPreset(p);
      m_settings.setValue("ui/nav", n);
      for (QAction* o : m_actions) if (o->objectName().startsWith("nav.")) o->setChecked(o->objectName() == "nav." + n.toLower());
    }, true);
    if (m_settings.value("ui/nav", "Fusion").toString() == name) { a->setChecked(true); m_viewport->setNavPreset(preset); }
  }
  for (const auto& [name, f, key] : std::vector<std::tuple<QString, Viewport::SelFilter, QString>>{{"Bodies", Viewport::SelFilter::Body, "1"}, {"Faces", Viewport::SelFilter::Face, "2"}, {"Edges", Viewport::SelFilter::Edge, "3"}, {"Vertices", Viewport::SelFilter::Vertex, "4"}}) {
    QAction* a = addAction("select." + name.toLower(), tr("Select: %1").arg(name), QKeySequence(key), [this, ff = f, n = name] {
      m_viewport->setSelectionFilter(ff);
      for (QAction* o : m_actions) if (o->objectName().startsWith("select.")) o->setChecked(o->objectName() == "select." + n.toLower());
    }, true);
    if (f == Viewport::SelFilter::Body) a->setChecked(true);
  }

  // Inspect
  addAction("inspect.distance", tr("Measure &distance (2 refs)"), QKeySequence("D"), [this] { measure("distance"); });
  addAction("inspect.angle", tr("Measure &angle (2 refs)"), QKeySequence("A"), [this] { measure("angle"); });
  addAction("inspect.radius", tr("Measure &radius (1 ref)"), QKeySequence("R"), [this] { measure("radius"); });
  addAction("inspect.bbox", tr("Measure &bounding box"), QKeySequence("B"), [this] { measure("bbox"); });
  m_pinAction = addAction("inspect.pin", tr("&Pin last measurement"), QKeySequence("P"), [this] { pinMeasurement(); });
  m_pinAction->setEnabled(false);
  addAction("inspect.properties", tr("Show &properties of selection"), QKeySequence("Ctrl+P"), [this] { showProperties(m_viewport->selection()); });

  // Annotate / edit
  addAction("annotate.add", tr("Add &annotation..."), QKeySequence("N"), [this] { addAnnotation(); });
  addAction("edit.rename", tr("&Rename..."), QKeySequence("F2"), [this] {
    auto ids = currentNodeIds();
    if (ids.empty()) return;
    m_browser->startRename(ids.front());
  });
  addAction("edit.hide", tr("&Hide selection"), QKeySequence("V"), [this] {
    for (const auto& id : currentNodeIds()) m_doc->run("appearance", opad::json{{"target", id}, {"visible", false}});
  });
  addAction("edit.showall", tr("Show all hidden objects"), QKeySequence("Shift+V"), [this] {
    for (const auto& [id, n] : m_doc->scene.nodes)
      if (!n.visible) m_doc->run("appearance", opad::json{{"target", id}, {"visible", true}});
  });
  addAction("edit.deleteop", tr("Delete selected objects (tombstone their import)"), QKeySequence::Delete, [this] {
    std::set<std::string> ops;
    for (const auto& id : currentNodeIds()) if (const opad::Node* n = m_doc->node(id)) ops.insert(n->source_op);
    if (ops.empty()) return;
    if (QMessageBox::question(this, tr("Delete"), tr("Tombstone %1 import operation(s)? History is kept; this can be undone from the timeline.").arg(ops.size())) != QMessageBox::Yes) return;
    for (const auto& op : ops) deleteOp(op);
  });

  // Tools
  addAction("tools.commands", tr("Command &search"), QKeySequence("S"), [this] {
    CommandPalette p(m_actions, this);
    p.move(mapToGlobal(QPoint(width() / 2 - 260, 120)));
    p.exec();
  });
  addAction("tools.shortcuts", tr("&Keyboard shortcuts..."), QKeySequence(), [this] { ShortcutEditor(m_actions, this).exec(); });
  addAction("tools.cache", tr("Clear tessellation &cache"), QKeySequence(), [this] {
    opad::json r = opad::commands::run("cache", opad::json{{"action", "clear"}});
    statusBar()->showMessage(tr("Cache cleared: %1").arg(QString::fromStdString(r["dir"].get<std::string>())), 4000);
  });
  addAction("help.about", tr("&About OPAD"), QKeySequence(), [this] {
    QMessageBox::about(this, tr("About OPAD"), tr("<b>OPAD %1</b><br>Git-native STEP viewer.<br>MIT licence. Built on Open CASCADE Technology and Qt.<br><br>Headless twin: <code>opad-cli</code>; Python: <code>import opad</code>.").arg(QString::fromStdString(opad::version_string())));
  });
}

QAction* find_action(const QList<QAction*>& list, const QString& id) {
  for (QAction* a : list) if (a->objectName() == id) return a;
  return nullptr;
}

void MainWindow::buildMenus() {
  auto add = [&](QMenu* m, std::initializer_list<const char*> ids) {
    for (const char* id : ids) {
      if (QString(id) == "-") { m->addSeparator(); continue; }
      if (QAction* a = find_action(m_actions, id)) m->addAction(a);
    }
  };
  QMenu* file = menuBar()->addMenu(tr("&File"));
  add(file, {"file.new", "file.open", "file.import", "-", "file.save", "file.saveas", "-", "file.export", "file.screenshot", "-", "file.quit"});
  QMenu* edit = menuBar()->addMenu(tr("&Edit"));
  add(edit, {"edit.rename", "edit.hide", "edit.showall", "edit.deleteop", "-", "annotate.add", "-", "select.bodies", "select.faces", "select.edges", "select.vertices"});
  QMenu* view = menuBar()->addMenu(tr("&View"));
  add(view, {"view.fit", "view.fitsel", "view.home", "-", "view.top", "view.front", "view.right", "view.iso", "view.bottom", "view.back", "view.left", "-", "view.ortho", "view.shaded", "view.edges", "view.wire", "view.grid", "view.shadows", "-", "view.section", "view.isolate", "view.unisolate", "-", "view.saveview"});
  m_viewsMenu = view->addMenu(tr("Named views"));
  view->addSeparator();
  QMenu* nav = view->addMenu(tr("Navigation preset"));
  add(nav, {"nav.fusion", "nav.solidworks", "nav.onshape", "nav.blender"});
  add(view, {"view.dark"});
  QMenu* inspect = menuBar()->addMenu(tr("&Inspect"));
  add(inspect, {"inspect.distance", "inspect.angle", "inspect.radius", "inspect.bbox", "inspect.pin", "-", "inspect.properties"});
  QMenu* tools = menuBar()->addMenu(tr("&Tools"));
  add(tools, {"tools.commands", "tools.shortcuts", "tools.cache"});
  QMenu* help = menuBar()->addMenu(tr("&Help"));
  add(help, {"help.about"});
}

void MainWindow::buildToolbar() {
  // Tabbed toolbar groups (F26): View, Inspect, Annotate, Export.
  auto* tabs = new QTabWidget(this);
  tabs->setDocumentMode(true);
  tabs->setMaximumHeight(72);
  auto makeTab = [&](const QString& title, std::initializer_list<const char*> ids) {
    auto* bar = new QToolBar(this);
    bar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    for (const char* id : ids) {
      if (QString(id) == "-") { bar->addSeparator(); continue; }
      if (QAction* a = find_action(m_actions, id)) {
        auto* b = new QToolButton(bar);
        b->setDefaultAction(a);
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        b->setAutoRaise(true);
        bar->addWidget(b);
      }
    }
    tabs->addTab(bar, title);
  };
  makeTab(tr("View"), {"view.fit", "view.home", "view.ortho", "-", "view.shaded", "view.edges", "view.wire", "view.grid", "-", "view.section", "view.isolate", "view.unisolate", "-", "select.bodies", "select.faces", "select.edges", "select.vertices"});
  makeTab(tr("Inspect"), {"inspect.distance", "inspect.angle", "inspect.radius", "inspect.bbox", "inspect.pin", "-", "inspect.properties"});
  makeTab(tr("Annotate"), {"annotate.add", "edit.rename", "edit.hide", "edit.showall", "view.saveview"});
  makeTab(tr("Export"), {"file.export", "file.screenshot", "file.import", "file.save"});
  auto* host = new QToolBar(tr("Ribbon"), this);
  host->setObjectName("ribbon");
  host->setMovable(false);
  host->addWidget(tabs);
  addToolBar(Qt::TopToolBarArea, host);
}

void MainWindow::buildDocks() {
  m_browser = new BrowserPanel(m_doc, this);
  auto* left = new QDockWidget(tr("Browser"), this);
  left->setObjectName("dock.browser");
  left->setWidget(m_browser);
  addDockWidget(Qt::LeftDockWidgetArea, left);

  m_props = new PropertiesPanel(this);
  auto* right = new QDockWidget(tr("Properties"), this);
  right->setObjectName("dock.properties");
  right->setWidget(m_props);
  addDockWidget(Qt::RightDockWidgetArea, right);

  m_annotations = new AnnotationsPanel(m_doc, this);
  auto* ann = new QDockWidget(tr("Annotations"), this);
  ann->setObjectName("dock.annotations");
  ann->setWidget(m_annotations);
  addDockWidget(Qt::RightDockWidgetArea, ann);
  tabifyDockWidget(right, ann);
  right->raise();

  m_timeline = new TimelineWidget(m_doc, this);
  auto* bottom = new QDockWidget(tr("Timeline"), this);
  bottom->setObjectName("dock.timeline");
  bottom->setWidget(m_timeline);
  bottom->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
  addDockWidget(Qt::BottomDockWidgetArea, bottom);
  resizeDocks({left, right}, {300, 320}, Qt::Horizontal);
  resizeDocks({bottom}, {48}, Qt::Vertical);
}

// ---------------------------------------------------------------- theme (F31)
void MainWindow::applyTheme(bool dark) {
  m_settings.setValue("ui/dark", dark);
  if (m_darkAction && m_darkAction->isChecked() != dark) m_darkAction->setChecked(dark);
  qApp->setStyle(QStyleFactory::create("Fusion"));
  QPalette p;
  if (dark) {
    p.setColor(QPalette::Window, QColor(43, 45, 48));
    p.setColor(QPalette::WindowText, QColor(230, 230, 230));
    p.setColor(QPalette::Base, QColor(32, 33, 36));
    p.setColor(QPalette::AlternateBase, QColor(43, 45, 48));
    p.setColor(QPalette::ToolTipBase, QColor(60, 62, 66));
    p.setColor(QPalette::ToolTipText, QColor(230, 230, 230));
    p.setColor(QPalette::Text, QColor(230, 230, 230));
    p.setColor(QPalette::Button, QColor(53, 55, 59));
    p.setColor(QPalette::ButtonText, QColor(230, 230, 230));
    p.setColor(QPalette::Highlight, QColor(30, 120, 210));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Mid, QColor(120, 120, 125));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(130, 130, 130));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(130, 130, 130));
  } else {
    p = QStyleFactory::create("Fusion")->standardPalette();
  }
  qApp->setPalette(p);
  m_viewport->setDarkTheme(dark);
}

void MainWindow::updateTitle() {
  setWindowTitle(m_doc->title());
  QString path = m_doc->hasDocument ? (m_doc->browse ? tr("Browsing (not saved)") : (m_doc->path().isEmpty() ? tr("Unsaved document") : m_doc->path())) : tr("No document. File > Open a .step or .opad file");
  if (!m_doc->scene.unresolved.empty()) path += tr("   |   %1 unresolved op(s)").arg(m_doc->scene.unresolved.size());
  m_statusPath->setText(path);
}

// ---------------------------------------------------------------- git status (F33)
void MainWindow::refreshGit() {
  if (!m_doc->hasDocument || m_doc->browse || m_doc->doc.path.empty()) {
    m_statusGit->clear();
    return;
  }
  QFileInfo fi(m_doc->path());
  QProcess git;
  git.setWorkingDirectory(fi.absolutePath());
  git.start("git", {"rev-parse", "--abbrev-ref", "HEAD"});
  if (!git.waitForFinished(800) || git.exitCode() != 0) {
    m_statusGit->setText(tr("not in git"));
    return;
  }
  QString branch = QString::fromUtf8(git.readAllStandardOutput()).trimmed();
  QProcess st;
  st.setWorkingDirectory(fi.absolutePath());
  st.start("git", {"status", "--porcelain", "--", fi.fileName()});
  st.waitForFinished(800);
  QString status = QString::fromUtf8(st.readAllStandardOutput()).trimmed();
  QString state = status.isEmpty() ? tr("clean") : status.startsWith("??") ? tr("untracked") : tr("modified");
  m_statusGit->setText(QString::fromUtf8("git: %1 · %2").arg(branch, state));
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
  for (const auto& r : refs) if (std::find(ids.begin(), ids.end(), r.body) == ids.end()) ids.push_back(r.body);
  m_browser->setSelectedIds(ids);
  showProperties(refs);
  m_statusSel->setText(refs.empty() ? QString() : tr("%1 selected").arg(refs.size()));
  writeSelectionFile();
  m_syncing = false;
}

void MainWindow::onBrowserSelection(const std::vector<std::string>& ids) {
  if (m_syncing) return;
  m_syncing = true;
  m_viewport->selectNodes(ids);
  std::vector<opad::Ref> refs;
  for (const auto& id : ids) { opad::Ref r; r.body = id; refs.push_back(r); }
  showProperties(refs);
  m_statusSel->setText(ids.empty() ? QString() : tr("%1 selected").arg(ids.size()));
  writeSelectionFile();
  m_syncing = false;
}

void MainWindow::showProperties(const std::vector<opad::Ref>& refs) {
  if (refs.empty()) {
    m_props->clear();
    return;
  }
  try {
    const opad::Ref& r = refs.front();
    opad::json j = r.kind == opad::Ref::Kind::Body ? opad::node_properties(m_doc->doc, m_doc->scene, r.body) : opad::inspect_ref(m_doc->doc, m_doc->scene, r);
    QString title = QString::fromStdString(r.str());
    if (r.kind == opad::Ref::Kind::Body) title = m_doc->nodeName(r.body);
    else if (r.kind != opad::Ref::Kind::Point) title = QString("%1 / %2 %3").arg(m_doc->nodeName(r.body)).arg(opad::Ref::kind_name(r.kind)).arg(r.index);
    if (refs.size() > 1) title += tr(" (+%1 more)").arg(refs.size() - 1);
    m_props->showJson(title, j);
  } catch (const std::exception& e) {
    m_props->showJson(tr("Error"), opad::json{{"error", e.what()}});
  }
}

// The live selection is published for agents (F25): opad-cli selection / opad.run("selection").
void MainWindow::writeSelectionFile() {
  try {
    opad::json j;
    j["pid"] = static_cast<long long>(QCoreApplication::applicationPid());
    j["document"] = m_doc->path().toStdString();
    j["browse"] = m_doc->browse;
    j["ts"] = opad::now_iso8601();
    opad::json sel = opad::json::array();
    for (const auto& r : m_viewport->selection()) {
      opad::json e;
      e["ref"] = r.str();
      e["node"] = m_doc->nodeName(r.body).toStdString();
      try {
        opad::json info = r.kind == opad::Ref::Kind::Body ? opad::node_properties(m_doc->doc, m_doc->scene, r.body) : opad::inspect_ref(m_doc->doc, m_doc->scene, r);
        for (const char* k : {"type", "surface", "curve", "bbox", "normal", "axis", "radius", "center", "area", "volume", "length", "key"})
          if (info.contains(k)) e[k] = info[k];
      } catch (const std::exception&) {
      }
      sel.push_back(e);
    }
    j["selection"] = sel;
    opad::write_text_file(opad::cache_dir() / "selection.json", j.dump(2));
  } catch (const std::exception&) {
  }
}

void MainWindow::showContextMenu(const QPoint& globalPos, std::vector<std::string> ids) {
  QMenu menu(this);
  auto add = [&](const char* id) { if (QAction* a = find_action(m_actions, id)) menu.addAction(a); };
  if (!ids.empty()) {
    menu.addSection(ids.size() == 1 ? m_doc->nodeName(ids.front()) : tr("%1 objects").arg(ids.size()));
    QAction* fit = menu.addAction(tr("Fit to"));
    connect(fit, &QAction::triggered, this, [this, ids] { m_viewport->fitNodes(ids); });
    add("view.isolate");
    QAction* hideOthers = menu.addAction(tr("Hide others"));
    connect(hideOthers, &QAction::triggered, this, [this, ids] {
      std::set<std::string> keep;
      for (const auto& id : ids) for (const auto& b : m_doc->scene.bodies_under(id)) keep.insert(b);
      for (const auto& b : m_doc->scene.all_bodies())
        if (!keep.count(b) && m_doc->node(b)->visible) m_doc->run("appearance", opad::json{{"target", b}, {"visible", false}});
    });
    add("edit.hide");
    add("edit.rename");
    QAction* color = menu.addAction(tr("Colour..."));
    connect(color, &QAction::triggered, this, [this, ids] {
      QColor c = QColorDialog::getColor(Qt::gray, this, tr("Colour"));
      if (!c.isValid()) return;
      for (const auto& id : ids) m_doc->run("appearance", opad::json{{"target", id}, {"color", {c.redF(), c.greenF(), c.blueF()}}});
    });
    const opad::Node* n = m_doc->node(ids.front());
    QAction* lock = menu.addAction(n && n->locked ? tr("Unlock") : tr("Lock"));
    connect(lock, &QAction::triggered, this, [this, ids, locked = n && n->locked] {
      for (const auto& id : ids) m_doc->run("appearance", opad::json{{"target", id}, {"locked", !locked}});
    });
    menu.addSeparator();
    add("annotate.add");
    add("inspect.distance");
    add("inspect.radius");
    add("inspect.properties");
    menu.addSeparator();
    add("edit.deleteop");
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

// ---------------------------------------------------------------- inspect (F23)
void MainWindow::measure(const QString& kind) {
  auto refs = m_viewport->selection();
  std::vector<std::string> strs;
  for (const auto& r : refs) strs.push_back(r.str());
  if (strs.empty()) for (const auto& id : m_browser->selectedIds()) strs.push_back(id);
  opad::json args{{"kind", kind.toStdString()}, {"refs", strs}};
  m_lastMeasure = opad::commands::run("measure", args, &m_doc->doc);
  m_props->showJson(tr("Measurement: %1").arg(kind), m_lastMeasure);
  m_pinAction->setEnabled(true);
  QString unit = QString::fromStdString(m_lastMeasure.value("unit", ""));
  if (m_lastMeasure.contains("value")) statusBar()->showMessage(tr("%1 = %2 %3  (P to pin)").arg(kind).arg(m_lastMeasure["value"].get<double>(), 0, 'g', 7).arg(unit), 10000);
  else statusBar()->showMessage(tr("%1 computed (see Properties)").arg(kind), 6000);
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
  m_doc->run("append", opad::json{{"op", op}});
  statusBar()->showMessage(tr("Measurement pinned to the document"), 4000);
}

void MainWindow::addAnnotation() {
  auto refs = m_viewport->selection();
  opad::Ref anchor;
  if (!refs.empty()) anchor = refs.front();
  else if (!m_browser->selectedIds().empty()) anchor.body = m_browser->selectedIds().front();
  else throw opad::Error("Select a body, face, edge or vertex to anchor the annotation.");
  bool ok = false;
  QString text = QInputDialog::getMultiLineText(this, tr("Annotation on %1").arg(QString::fromStdString(anchor.str())), tr("Note:"), QString(), &ok);
  if (!ok || text.trimmed().isEmpty()) return;
  m_doc->run("annotate", opad::json{{"anchor", anchor.str()}, {"text", text.toStdString()}});
}

void MainWindow::deleteOp(const std::string& opId) {
  m_doc->run("delete", opad::json{{"target", opId}});
}

void MainWindow::selectOpTargets(const std::string& opId) {
  const opad::Op* op = m_doc->doc.find_op(opId);
  if (!op) return;
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
  m_props->showJson(tr("Operation %1").arg(QString::fromStdString(op->type)), d);
}

// ---------------------------------------------------------------- export (F14/F15)
void MainWindow::exportDialog() {
  if (!m_doc->hasDocument) throw opad::Error("Nothing to export.");
  QDialog dlg(this);
  dlg.setWindowTitle(tr("Export"));
  auto* form = new QFormLayout(&dlg);
  auto* format = new QComboBox(&dlg);
  for (const auto& f : opad::commands::exporter_formats()) format->addItem(QString::fromStdString(f));
  auto* scope = new QComboBox(&dlg);
  auto ids = currentNodeIds();
  scope->addItem(tr("Whole document"));
  if (!ids.empty()) { scope->addItem(tr("Selection (%1 object(s))").arg(ids.size())); scope->setCurrentIndex(1); }
  auto* schema = new QComboBox(&dlg);
  schema->addItems({"AP214", "AP242", "AP203"});
  auto* tol = new QDoubleSpinBox(&dlg);
  tol->setRange(0.001, 10);
  tol->setDecimals(3);
  tol->setValue(0.1);
  tol->setSuffix(" mm");
  auto* ascii = new QCheckBox(tr("ASCII STL"), &dlg);
  auto* perBody = new QCheckBox(tr("One STL file per body"), &dlg);
  auto* mtl = new QCheckBox(tr("Write OBJ material library"), &dlg);
  mtl->setChecked(true);
  form->addRow(tr("Format"), format);
  form->addRow(tr("Objects"), scope);
  form->addRow(tr("STEP schema"), schema);
  form->addRow(tr("Mesh tolerance"), tol);
  form->addRow(ascii);
  form->addRow(perBody);
  form->addRow(mtl);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  form->addRow(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  if (dlg.exec() != QDialog::Accepted) return;
  QString fmt = format->currentText();
  QString out = QFileDialog::getSaveFileName(this, tr("Export %1").arg(fmt), m_settings.value("ui/lastDir").toString(), QString("%1 (*.%1)").arg(fmt));
  if (out.isEmpty()) return;
  if (!out.endsWith("." + fmt, Qt::CaseInsensitive) && !(fmt == "step" && out.endsWith(".stp", Qt::CaseInsensitive))) out += "." + fmt;
  opad::json args{{"format", fmt.toStdString()}, {"out", out.toStdString()}, {"schema", schema->currentText().toStdString()}, {"tolerance", tol->value()},
                  {"ascii", ascii->isChecked()}, {"per_body", perBody->isChecked()}, {"mtl", mtl->isChecked()}};
  if (scope->currentIndex() == 1) args["select"] = ids;
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

// ---------------------------------------------------------------- section (F20)
void MainWindow::sectionDialog() {
  if (!m_sectionDialog) {
    m_sectionDialog = new QDialog(this);
    m_sectionDialog->setWindowTitle(tr("Section analysis"));
    auto* form = new QFormLayout(m_sectionDialog);
    m_sectionOn = new QCheckBox(tr("Enable section plane"), m_sectionDialog);
    m_sectionAxis = new QComboBox(m_sectionDialog);
    m_sectionAxis->addItems({"X", "Y", "Z"});
    m_sectionAxis->setCurrentIndex(2);
    m_sectionSlider = new QSlider(Qt::Horizontal, m_sectionDialog);
    m_sectionSlider->setRange(0, 1000);
    m_sectionSlider->setValue(500);
    m_sectionFlip = new QCheckBox(tr("Flip side"), m_sectionDialog);
    auto* save = new QPushButton(tr("Save as named section..."), m_sectionDialog);
    form->addRow(m_sectionOn);
    form->addRow(tr("Axis"), m_sectionAxis);
    form->addRow(tr("Offset"), m_sectionSlider);
    form->addRow(m_sectionFlip);
    form->addRow(save);
    auto apply = [this] {
      opad::Vec3 lo, hi;
      if (!opad::scene_bbox(m_doc->doc, m_doc->scene, {}, lo, hi)) return;
      int axis = m_sectionAxis->currentIndex();
      double t = m_sectionSlider->value() / 1000.0;
      opad::Vec3 origin{(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
      origin[axis] = lo[axis] + t * (hi[axis] - lo[axis]);
      opad::Vec3 normal{0, 0, 0};
      normal[axis] = m_sectionFlip->isChecked() ? 1 : -1;
      m_viewport->setSection(m_sectionOn->isChecked(), origin, normal);
    };
    connect(m_sectionOn, &QCheckBox::toggled, this, apply);
    connect(m_sectionAxis, &QComboBox::currentIndexChanged, this, apply);
    connect(m_sectionSlider, &QSlider::valueChanged, this, apply);
    connect(m_sectionFlip, &QCheckBox::toggled, this, apply);
    connect(save, &QPushButton::clicked, this, [this, apply] {
      apply();
      bool ok = false;
      QString name = QInputDialog::getText(this, tr("Named section"), tr("Name:"), QLineEdit::Normal, tr("Section %1").arg(m_doc->scene.sections.size() + 1), &ok);
      if (!ok || name.isEmpty()) return;
      opad::Vec3 lo, hi;
      opad::scene_bbox(m_doc->doc, m_doc->scene, {}, lo, hi);
      int axis = m_sectionAxis->currentIndex();
      opad::Vec3 origin{(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
      origin[axis] = lo[axis] + m_sectionSlider->value() / 1000.0 * (hi[axis] - lo[axis]);
      opad::Vec3 normal{0, 0, 0};
      normal[axis] = m_sectionFlip->isChecked() ? 1 : -1;
      guarded([&] { m_doc->run("section", opad::json{{"name", name.toStdString()}, {"origin", origin}, {"normal", normal}}); });
    });
  }
  m_sectionOn->setChecked(true);
  m_sectionDialog->show();
  m_sectionDialog->raise();
}

// ---------------------------------------------------------------- named views (F6 view op)
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
    QAction* a = m_viewsMenu->addAction(QString::fromStdString(v.name));
    connect(a, &QAction::triggered, this, [this, id = v.id] { restoreNamedView(id); });
  }
  if (!m_doc->scene.sections.empty()) {
    m_viewsMenu->addSeparator();
    for (const auto& s : m_doc->scene.sections) {
      QAction* a = m_viewsMenu->addAction(tr("Section: %1").arg(QString::fromStdString(s.name)));
      connect(a, &QAction::triggered, this, [this, s] { m_viewport->setSection(true, s.origin, s.normal); });
    }
  }
  if (m_viewsMenu->isEmpty()) m_viewsMenu->addAction(tr("(none saved)"))->setEnabled(false);
}

// ---------------------------------------------------------------- lifecycle
void MainWindow::openPath(const QString& path) {
  guarded([&] {
    m_settings.setValue("ui/lastDir", QFileInfo(path).absolutePath());
    m_doc->open(path);
    m_viewport->fitAll();
  });
}

bool MainWindow::maybeSave() {
  if (!m_doc->isDirty()) return true;
  auto r = QMessageBox::question(this, tr("Unsaved changes"), tr("Save changes to %1?").arg(m_doc->path().isEmpty() ? tr("the document") : m_doc->path()),
                                 QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
  if (r == QMessageBox::Cancel) return false;
  if (r == QMessageBox::Save) {
    try {
      if (m_doc->doc.path.empty()) find_action(m_actions, "file.saveas")->trigger();
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
  e->accept();
}
