// Menus, the ribbon (workspaces, tabs, its settings menu) and the Tools and Help commands.
#include "MainWindow.hpp"
#include "AgentBridge.hpp"
#include "FileAssociations.hpp"
#include "RecoveryManager.hpp"

#include <QActionGroup>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QInputDialog>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>

#include "I18n.hpp"
#include "Icons.hpp"
#include "opad/design/feature.hpp"

void MainWindow::buildToolsActions() {
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
  add(edit, {"edit.undo", "edit.redo", "-", "edit.rename", "edit.hide", "edit.showall", "edit.filter", "edit.selectparent", "-", "annotate.add", "annotate.draw", "annotate.resolve", "annotate.show", "-", "edit.delete", "edit.restore", "edit.selecttouched", "-", "select.bodies", "select.faces", "select.edges", "select.vertices"});
  QMenu* view = menuBar()->addMenu(tr("&View"));
  add(view, {"view.fit", "view.fitall", "view.home", "view.rollleft", "view.rollright", "-", "view.top", "view.front", "view.right", "view.iso", "view.bottom", "view.back", "view.left", "-", "view.ortho", "view.shaded", "view.edges", "view.wire", "view.grid", "view.gridSettings", "select.through", "-", "view.isolate", "view.unisolate", "-", "view.saveview"});
  m_viewsMenu = view->addMenu(tr("Named views"));
  view->addSeparator();
  QMenu* nav = view->addMenu(tr("Navigation preset"));
  add(nav, {"nav.fusion", "nav.solidworks", "nav.onshape", "nav.blender"});
  add(view, {"view.dark", "-", "workspace.review", "workspace.design", "-", "panel.browser", "panel.annotations", "panel.section", "panel.timeline", "panel.reset"});
  QMenu* inspect = menuBar()->addMenu(tr("&Inspect"));
  add(inspect, {"inspect.distance", "inspect.angle", "inspect.radius", "inspect.bbox", "inspect.pin", "inspect.clear", "-", "inspect.properties", "select.geometry", "-", "inspect.interference", "inspect.printcheck", "-", "inspect.section", "inspect.flip"});
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
  const QMap<QString, QMenu*> menus{{"file", file}, {"edit", edit}, {"view", view}, {"inspect", inspect}, {"design", designMenu}, {"tools", tools}, {"help", help}};
  for (AreaController* area : m_areas) area->menus(menuBar(), menus);
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
  RibbonLayout layout;
  layout.addWorkspace("review", {tr("Review"), "eye", "Ctrl+1", tr("Look, measure, annotate. Nothing here changes geometry or structure."), tr("ops: annotation · measurement · section · view")});
  layout.addWorkspace("design", {tr("Design"), "component", "Ctrl+2", tr("Model parts: sketches, features, parameters; arrange the assembly."), tr("ops: param · sketch · feature · edit · regen · import · reparent · appearance")});
  Workspace sketchWs{tr("Sketch"), "sketch", "", tr("Drawing a sketch. Finish sketch returns to Design."), tr("ops: sketch · edit")};
  sketchWs.contextual = true;
  layout.addWorkspace("sketch", sketchWs);
  layout.addTab("review", "review.view", tr("View"), {acts({"view.fit", "view.home", "view.ortho", "view.2d"}), acts({"view.shaded", "view.edges", "view.wire", "view.grid", "view.gridSettings", "select.through"}), acts({"view.isolate", "view.unisolate"})});
  layout.addTab("review", "review.inspect", tr("Inspect"), {acts({"inspect.distance", "inspect.angle", "inspect.radius", "inspect.bbox"}), acts({"inspect.pin", "inspect.properties"}), acts({"inspect.interference", "inspect.printcheck"}), acts({"inspect.section", "inspect.flip"})});
  layout.addTab("review", "review.annotate", tr("Annotate"), {acts({"panel.annotations", "annotate.add", "annotate.draw", "annotate.resolve", "annotate.show"}), acts({"edit.rename", "edit.hide", "edit.showall", "view.saveview"})});
  layout.addTab("review", "review.export", tr("Export"), {acts({"file.export", "file.screenshot"}), acts({"file.import", "file.save"})});
  layout.addTab("design", "design.solid", tr("Solid"), {acts({"design.sketch", "design.extrude", "design.revolve", "design.sweep", "design.loft", "design.hole", "design.pipe", "design.coil"}),
                                         acts({"design.box", "design.cylinder", "design.sphere", "design.cone", "design.torus"}), acts({"design.parameters"})});
  layout.addTab("design", "design.modify", tr("Modify"), {acts({"design.offset_face", "design.thicken", "design.fillet", "design.chamfer", "design.shell", "design.draft", "design.scale"}),
                                          acts({"design.combine", "design.split", "design.move", "design.remove"}),
                                          acts({"design.mirror", "design.pattern_rect", "design.pattern_circ"})});
  layout.addTab("design", "design.construct", tr("Construct"), {acts({"design.plane", "design.axis", "design.interference"}), acts({"design.parameters", "design.edit", "design.regenerate"})});
  layout.addTab("design", "design.assemble", tr("Assemble"), {acts({"file.import", "design.newcomponent", "design.reparent"}), acts({"edit.rename", "edit.delete", "edit.restore"}),
                                            acts({"design.colour", "design.opacity", "design.lock", "edit.hide", "view.isolate"})});
  layout.addTab("design","design.view",tr("View"),{acts({"view.fit","view.home","view.2d","view.ortho"}),acts({"view.shaded","view.edges","view.wire","view.grid","view.gridSettings","select.through"})});
  layout.addTab("design", "design.export", tr("Export"), {acts({"file.export", "file.screenshot"}), acts({"file.import", "file.save"})});
  layout.addTab("sketch", "sketch.create", tr("Create"), {acts({"sketch.finish", "sketch.cancel", "view.2d", "view.alignPlane"}),
      acts({"sketch.line", "sketch.rect", "sketch.circle", "sketch.arc3", "sketch.spline", "sketch.ellipse", "sketch.slot", "sketch.polygon", "sketch.point", "sketch.moreCreate"})});
  layout.addTab("sketch", "sketch.modify", tr("Modify"), {acts({"sketch.finish", "view.2d"}),
      acts({"sketch.select", "sketch.trim", "sketch.fillet", "sketch.offset", "sketch.mirror", "sketch.construction", "sketch.node", "sketch.openEnds", "sketch.moreModify"})});
  layout.addTab("sketch", "sketch.constrain", tr("Constrain"), {acts({"sketch.finish", "sketch.constraints", "sketch.dimension"}),
      acts({"sketch.c.horizontal", "sketch.c.vertical", "sketch.c.coincident", "sketch.c.parallel", "sketch.c.perpendicular", "sketch.c.tangent", "sketch.c.fix", "sketch.moreConstrain"})});
  layout.addTab("sketch", "sketch.reference", tr("Reference"), {acts({"sketch.finish", "sketch.moreReference", "sketch.moreFiles", "sketch.snaps", "sketch.selectionOptions"}),
      acts({"sketch.project", "sketch.replane", "design.parameters", "view.grid", "view.gridSettings"})});
  for (AreaController* area : m_areas) area->ribbon(layout);  // their workspaces, tabs and groups
  for (const RibbonLayout::Space& space : layout.spaces) {
    const int index = m_ribbon->addWorkspace(space.workspace);
    for (const RibbonLayout::Tab& tab : space.tabs) {
      QList<QList<QAction*>> groups;
      for (QList<QAction*> group : tab.groups) {
        group.removeAll(nullptr);
        if (!group.isEmpty()) groups << group;
      }
      m_ribbon->addTab(index, tab.title, groups);
    }
  }
  const int review = layout.index("review"), design = layout.index("design");
  m_sketchWorkspace = layout.index("sketch");
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
  settings->addAction(tr("Autosave and recovery"),this,[this]{m_recovery->settings();});
  settings->addAction(tr("Recover documents"),this,[this]{m_recovery->offerRecovery();});
  settings->addAction(tr("AI integration"),this,[this]{m_agent->settings();});
  settings->addAction(tr("Agent activity"),this,[this]{m_agent->showActivity();});
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
  settings->addAction(tr("Hover highlighting"),this,[this]{
    QDialog dialog(this);dialog.setWindowTitle(tr("Hover highlighting"));auto* layout=new QVBoxLayout(&dialog);
    auto* enabled=new QCheckBox(tr("Fade hover highlight"),&dialog);enabled->setChecked(m_settings.value("view/hoverFade",true).toBool());layout->addWidget(enabled);
    auto* seconds=new QDoubleSpinBox(&dialog);seconds->setRange(.1,60);seconds->setSuffix(tr(" seconds"));seconds->setValue(m_settings.value("view/hoverFadeSeconds",5).toDouble());layout->addWidget(seconds);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);layout->addWidget(buttons);connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if(dialog.exec()==QDialog::Accepted)m_viewport->setHoverFade(enabled->isChecked(),seconds->value());
  });
  for (auto* a : quality->actions()) a->setToolTip(tr("Ray tracing requires a compatible OpenGL driver; Studio is used when unavailable."));
  auto* background = settings->addMenu(tr("Scene background"));
  auto* backgroundGroup = new QActionGroup(background);
  const QStringList backgrounds = {tr("Theme"), tr("Studio gradient"), tr("White"), tr("Dark slate")};
  for (int i = 0; i < backgrounds.size(); ++i) {
    auto* a = background->addAction(backgrounds[i]);
    a->setCheckable(true); backgroundGroup->addAction(a);
    a->setChecked(Viewport::savedSceneBackground() == i);
    connect(a, &QAction::triggered, this, [this, i] { m_viewport->setSceneBackground(i); });
  }
  QMenu* navMenu = settings->addMenu(tr("Navigation preset"));
  for (QAction* a : m_actions) if (a->objectName().startsWith("nav.")) navMenu->addAction(a);
  settings->addSeparator();
  // Viewer mode for STEP, STL, DXF and the rest; off, they open as editable, unsaved documents (slower: prepared for saving).
  auto* viewerMode = settings->addAction(tr("Open other formats read-only (viewer mode)"));
  viewerMode->setCheckable(true);
  viewerMode->setChecked(m_doc->viewerOpens);
  viewerMode->setToolTip(tr("STEP, IGES, STL, 3MF, OBJ, DXF, SVG and the other formats open read-only and fast; Save makes them editable OPAD documents."));
  connect(viewerMode, &QAction::toggled, this, [this](bool on) { m_doc->viewerOpens = on; m_settings.setValue("files/viewerMode", on); });
  if (associations::supported()) settings->addAction(tr("File types…"), this, [this] { FileTypesDialog(this).exec(); });
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
