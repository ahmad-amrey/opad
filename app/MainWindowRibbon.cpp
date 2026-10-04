// Menus, the ribbon (workspaces, tabs, its settings menu) and the Tools and Help commands.
#include "MainWindow.hpp"
#include "KeyText.hpp"
#include "AgentBridge.hpp"
#include "FileAssociations.hpp"
#include "Legal.hpp"
#include "RecoveryManager.hpp"

#include <QActionGroup>
#include <QDir>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QToolBar>

#include "GuidedTool.hpp"
#include "FileLocation.hpp"
#include "I18n.hpp"
#include "Preferences.hpp"
#include "Icons.hpp"
#include "opad/design/feature.hpp"

void MainWindow::buildToolsActions() {
  addAction("tools.commands", tr("Search commands"), "search", QKeySequence("S"), [this] {
    CommandPalette p(m_actions, this);
    p.adjustSize();
    p.move(mapToGlobal(QPoint((width() - p.width()) / 2, 180)));
    p.exec();
  });
  addAction("tools.shortcuts", tr("Keyboard shortcuts…"), "", QKeySequence("Ctrl+K"), [this] { ShortcutEditor(m_actions, this).exec(); });
  // Both are rows of Preferences now (UI-110): the commands open it there.
  addAction("tools.author", tr("Annotation author..."), "", QKeySequence(), [this] { PreferencesDialog::open(this, "general", "user/name"); });
  m_doc->setUndoLimit(m_settings.value("edit/undoDepth", 50).toInt());
  addAction("tools.undodepth", tr("Undo history…"), "", QKeySequence(), [this] { PreferencesDialog::open(this, "general", "edit/undoDepth"); });
  addAction("tools.cache", tr("Clear tessellation cache"), "", QKeySequence(), [this] {
    opad::json r = opad::commands::run("cache", opad::json{{"action", "clear"}});
    const QString dir = QString::fromStdString(r["dir"].get<std::string>());
    resultToast(tr("Cache cleared: %1").arg(QDir::toNativeSeparators(dir)), dir);
  });
  auto help = [this](const QString& id, const QString& label, const QStringList& keywords, std::function<void()> fn) {
    CommandInfo info;  // with the words the palette finds it by
    info.id = id;
    info.label = label;
    info.keywords = keywords;
    return addCommand(info, std::move(fn));
  };
  help("help.about", tr("&About OPAD"), {"version", "licence", "license", "copyright", "trademarks"}, [this] { legal::showAbout(this); });
  help("help.licenses", tr("Third-party licences…"), {"licenses", "notices", "open source", "copyright", "GPL", "LGPL", "MIT"}, [this] { legal::showNotices(this); });
  help("help.aboutqt", tr("About Qt"), {"Qt", "licence", "license", "version"}, [this] { QMessageBox::aboutQt(this); })->setMenuRole(QAction::AboutQtRole);
}

void MainWindow::buildMenus() {
  auto add = [&](QMenu* m, std::initializer_list<const char*> ids) {
    for (const char* id : ids) {
      if (QString(id) == "-") { m->addSeparator(); continue; }
      if (QAction* a = action(id)) m->addAction(a);
    }
  };
  QMenu* file = menuBar()->addMenu(tr("&File"));
  add(file, {"file.new"});
  m_templateMenu = file->addMenu(icons::themed("template", 16), tr("New from template"));
  m_templateMenu->setObjectName("templates");
  connect(m_templateMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildTemplateMenu);
  add(file, {"file.open", "file.import", "file.importdoc"});
  m_recentMenu = file->addMenu(tr("Recent"));
  m_recentMenu->setObjectName("recent");
  location::addContextMenus(m_recentMenu, [this](const QString& path, QWidget* parent) { return recentMenu(path, parent); });
  add(file, {"-", "file.close", "-", "file.save", "file.saveas", "file.savetemplate", "-", "file.export", "file.screenshot", "-", "file.quit"});
  QMenu* edit = menuBar()->addMenu(tr("&Edit"));
  add(edit, {"edit.undo", "edit.redo", "edit.repeat", "-", "edit.selectall", "edit.invert", "edit.selectparent", "-", "edit.rename", "edit.hide", "edit.showall", "edit.filter", "-", "annotate.add", "annotate.draw", "annotate.resolve", "annotate.show", "-", "edit.delete", "edit.restore", "edit.selecttouched", "-", "select.bodies", "select.faces", "select.edges", "select.vertices"});
  QMenu* view = m_viewMenu = menuBar()->addMenu(tr("&View"));
  add(view, {"view.fit", "view.fitall", "view.home", "view.rollleft", "view.rollright", "-", "view.top", "view.front", "view.right", "view.iso", "view.bottom", "view.back", "view.left", "-", "view.ortho", "view.shaded", "view.edges", "view.wire", "view.grid", "view.gridSettings", "select.through", "-", "view.isolate", "view.unisolate", "-", "view.saveview"});
  m_viewsMenu = view->addMenu(tr("Named views"));
  m_viewsMenu->setObjectName("views");
  view->addSeparator();
  QMenu* nav = view->addMenu(tr("Navigation preset"));
  nav->setObjectName("navigation");
  add(nav, {"nav.fusion", "nav.solidworks", "nav.onshape", "nav.blender"});
  add(view, {"view.dark", "-", "workspace.review", "workspace.design", "-", "panel.browser", "panel.annotations", "panel.section", "panel.timeline", "panel.reset"});
  QMenu* inspect = menuBar()->addMenu(tr("&Inspect"));
  add(inspect, {"inspect.distance", "inspect.angle", "inspect.radius", "inspect.bbox", "inspect.pin", "inspect.clear", "-", "inspect.properties", "select.similar", "-", "inspect.interference", "inspect.printcheck", "-", "inspect.section", "inspect.flip"});
  QMenu* designMenu = menuBar()->addMenu(tr("&Design"));
  add(designMenu, {"design.sketch", "design.convertDrawing", "design.parameters", "-"});
  for (const char* group : {"create", "modify", "combine", "pattern", "body", "construct"}) {
    QMenu* sub = designMenu->addMenu(i18n::t(QString(group).left(1).toUpper() + QString(group).mid(1)));
    sub->setObjectName(group);
    for (const auto& spec : opad::design::feature_specs())
      if (spec.group == group) sub->addAction(action("design." + QString::fromStdString(spec.kind)));
  }
  add(designMenu, {"-", "design.edit", "design.regenerate", "-", "design.newcomponent", "design.reparent", "design.colour", "design.opacity", "design.lock"});
  QMenu* tools = menuBar()->addMenu(tr("&Tools"));
  add(tools, {"tools.commands", "tools.shortcuts", "tools.cache"});
  QMenu* help = menuBar()->addMenu(tr("&Help"));
  add(help, {"help.licenses", "help.aboutqt", "-", "help.about"});
  const QMap<QString, QMenu*> menus{{"file", file}, {"edit", edit}, {"view", view}, {"inspect", inspect}, {"design", designMenu}, {"tools", tools}, {"help", help}};
  for (AreaController* area : m_areas) area->menus(menuBar(), menus);
  rebuildRecentMenu();
  // Each command's menu path, by menu ids ("design/create"): a top menu by its key above (an area's own by its title), a
  // submenu by its object name, else its title.
  const std::function<void(QMenu*, const QString&)> record = [&](QMenu* menu, const QString& path) {
    for (QAction* a : menu->actions()) {
      if (QMenu* sub = a->menu()) record(sub, path + "/" + (sub->objectName().isEmpty() ? QString(sub->title()).remove('&').toLower() : sub->objectName()));
      if (!a->objectName().isEmpty()) m_commands.setMenuPath(a->objectName(), path);
    }
  };
  for (QAction* top : menuBar()->actions())
    if (QMenu* menu = top->menu()) record(menu, menus.key(menu, QString(menu->title()).remove('&').toLower()));
}

bool MainWindow::setContextualTab(const QString& id, bool shown) { return m_ribbon->setContextualTab(id, shown); }

// The keys that switch workspace now, for the status bar: "Ctrl+1 / 2" (the modifiers of the first said once), the
// workspaces without a key left out; "" when none has one.
QString MainWindow::workspaceKeys() const {
  QString out, modifiers;
  for (const QString& id : m_workspaceIds) {
    const QString key = keys::plain(keys::binding(action("workspace." + id)));
    if (key.isEmpty()) continue;
    if (out.isEmpty()) {
      out = key;
      modifiers = key.left(key.lastIndexOf('+') + 1);
    } else {
      const bool same = !modifiers.isEmpty() && key.startsWith(modifiers) && key.lastIndexOf('+') + 1 == modifiers.size();
      out += " / " + (same ? key.mid(modifiers.size()) : key);
    }
  }
  return keys::isolate(out);
}

void MainWindow::setWorkspace(const QString& id) {
  if (const int i = m_workspaceIds.indexOf(id); i != m_sketchWorkspace) m_ribbon->setWorkspace(i);  // the sketch's: updateDesignState
}

void MainWindow::buildRibbon() {
  m_ribbon = new RibbonBar(this);
  auto acts = [&](std::initializer_list<const char*> ids) {
    QList<QAction*> out;
    for (const char* id : ids) if (QAction* a = action(id)) out << a;
    return out;
  };
  RibbonLayout layout;
  layout.addWorkspace("review", {tr("Review"), "eye", "Ctrl+1", tr("Look, measure, annotate. Apart from Import, nothing here changes geometry or structure."), tr("ops: annotation · measurement · section · view")});
  layout.addWorkspace("design", {tr("Design"), "component", "Ctrl+2", tr("Model parts: sketches, features, parameters; arrange the assembly."), tr("ops: param · sketch · feature · edit · regen · import · reparent · appearance")});
  Workspace sketchWs{tr("Sketch"), "sketch", "", tr("Drawing a sketch. Finish sketch returns to Design."), tr("ops: sketch · edit")};
  sketchWs.contextual = true;
  layout.addWorkspace("sketch", sketchWs);
  // The built-in tabs: titled groups of large tools (UI-120 b), the same tools in the same order as before.
  auto group = [&](const QString& tab, const QString& name, const QString& title, std::initializer_list<const char*> ids) {
    layout.addGroup(tab, tab + "." + name, title);
    for (const char* id : ids) layout.addAction(tab + "." + name, action(id));
  };
  layout.addTab("review", "review.view", tr("View"));
  group("review.view", "navigate", tr("Navigate"), {"view.fit", "view.home", "view.ortho", "view.2d"});
  group("review.view", "display", tr("Display"), {"view.shaded", "view.edges", "view.wire", "view.grid", "view.gridSettings", "select.through"});
  group("review.view", "isolate", tr("Isolate"), {"view.isolate", "view.unisolate"});
  layout.addTab("review", "review.inspect", tr("Inspect"));
  group("review.inspect", "measure", tr("Measure"), {"inspect.distance", "inspect.angle", "inspect.radius", "inspect.bbox"});
  group("review.inspect", "results", tr("Results"), {"inspect.pin", "inspect.properties"});
  group("review.inspect", "check", tr("Check"), {"inspect.interference", "inspect.printcheck"});
  group("review.inspect", "section", tr("Section"), {"inspect.section", "inspect.flip"});
  layout.addTab("review", "review.annotate", tr("Annotate"));
  group("review.annotate", "markup", tr("Markup"), {"panel.annotations", "annotate.add", "annotate.draw", "annotate.resolve", "annotate.show"});
  group("review.annotate", "objects", tr("Objects"), {"edit.rename", "edit.hide", "edit.showall", "view.saveview"});
  layout.addTab("review", "review.export", tr("Export"));
  group("review.export", "export", tr("Export"), {"file.export", "file.screenshot"});
  group("review.export", "file", tr("File"), {"file.import", "file.save"});
  layout.addTab("design", "design.solid", tr("Solid"));
  group("design.solid", "create", tr("Create"), {"design.sketch", "design.extrude", "design.revolve", "design.sweep", "design.loft", "design.hole", "design.pipe", "design.coil"});
  group("design.solid", "primitives", tr("Primitives"), {"design.box", "design.cylinder", "design.sphere", "design.cone", "design.torus"});
  group("design.solid", "parameters", tr("Parameters"), {"design.parameters"});
  layout.addTab("design", "design.modify", tr("Modify"));
  group("design.modify", "modify", tr("Modify"), {"design.offset_face", "design.thicken", "design.fillet", "design.chamfer", "design.shell", "design.draft", "design.scale"});
  group("design.modify", "combine", tr("Combine"), {"design.combine", "design.split", "design.move", "design.remove"});
  group("design.modify", "pattern", tr("Pattern"), {"design.mirror", "design.pattern_rect", "design.pattern_circ"});
  layout.addTab("design", "design.construct", tr("Construct"));
  group("design.construct", "construct", tr("Construct"), {"design.plane", "design.axis", "design.interference"});
  group("design.construct", "history", tr("History"), {"design.parameters", "design.edit", "design.regenerate"});
  layout.addTab("design", "design.assemble", tr("Assemble"));
  group("design.assemble", "components", tr("Components"), {"file.import", "design.newcomponent", "design.reparent"});
  group("design.assemble", "edit", tr("Edit"), {"edit.rename", "edit.delete", "edit.restore"});
  group("design.assemble", "appearance", tr("Appearance"), {"design.colour", "design.opacity", "design.lock", "edit.hide", "view.isolate"});
  layout.addTab("design", "design.view", tr("View"));
  group("design.view", "navigate", tr("Navigate"), {"view.fit", "view.home", "view.2d", "view.ortho"});
  group("design.view", "display", tr("Display"), {"view.shaded", "view.edges", "view.wire", "view.grid", "view.gridSettings", "select.through"});
  layout.addTab("design", "design.export", tr("Export"));
  group("design.export", "export", tr("Export"), {"file.export", "file.screenshot"});
  group("design.export", "file", tr("File"), {"file.import", "file.save"});
  layout.addTab("sketch", "sketch.create", tr("Create"));
  group("sketch.create", "sketch", tr("Sketch"), {"sketch.finish", "sketch.cancel", "view.2d", "view.alignPlane"});
  group("sketch.create", "draw", tr("Draw"), {"sketch.line", "sketch.rect", "sketch.circle", "sketch.arc3", "sketch.spline", "sketch.ellipse", "sketch.slot", "sketch.polygon", "sketch.point", "sketch.moreCreate"});
  layout.addTab("sketch", "sketch.modify", tr("Modify"));
  group("sketch.modify", "sketch", tr("Sketch"), {"sketch.finish", "view.2d"});
  group("sketch.modify", "modify", tr("Modify"), {"sketch.select", "sketch.trim", "sketch.fillet", "sketch.offset", "sketch.mirror", "sketch.construction", "sketch.node", "sketch.openEnds", "sketch.moreModify"});
  layout.addTab("sketch", "sketch.constrain", tr("Constrain"));
  group("sketch.constrain", "sketch", tr("Sketch"), {"sketch.finish", "sketch.constraints", "sketch.dimension"});
  group("sketch.constrain", "constraints", tr("Constraints"), {"sketch.c.horizontal", "sketch.c.vertical", "sketch.c.coincident", "sketch.c.parallel", "sketch.c.perpendicular", "sketch.c.tangent", "sketch.c.fix", "sketch.moreConstrain"});
  layout.addTab("sketch", "sketch.reference", tr("Reference"));
  group("sketch.reference", "sketch", tr("Sketch"), {"sketch.finish", "sketch.moreReference", "sketch.moreFiles", "sketch.snaps", "sketch.selectionOptions"});
  group("sketch.reference", "reference", tr("Reference"), {"sketch.project", "sketch.replane", "design.parameters", "view.grid", "view.gridSettings"});
  for (AreaController* area : m_areas) area->ribbon(layout);  // their workspaces, tabs and groups
  for (const RibbonLayout::Space& space : layout.spaces) {
    Workspace shown = space.workspace;
    if (!shown.contextual) shown.command = "workspace." + space.id;  // the chip and its list show that command's key now
    const int index = m_ribbon->addWorkspace(shown);
    m_workspaceIds << space.id;
    for (const RibbonLayout::Tab& tab : space.tabs) {
      for (const RibbonLayout::Group& group : tab.groups)
        for (const RibbonLayout::Item& item : group.items)
          for (QAction* a : QList<QAction*>{item.action} + item.variants)
            if (a) m_commands.addWorkspace(a->objectName(), space.id);
      m_ribbon->addTab(index, tab);
    }
    if (space.workspace.contextual) continue;
    const QString key = space.workspace.key;
    QAction* a = action("workspace." + space.id);
    if (!a) {  // an area's workspace: its command beside the others in the View menu
      a = addAction("workspace." + space.id, tr("%1 workspace").arg(space.workspace.name), space.workspace.icon, QKeySequence(key), [this, id = space.id] { setWorkspace(id); }, true);
      const QList<QAction*> entries = m_viewMenu->actions();
      int last = -1;
      for (int i = 0; i < entries.size(); ++i)
        if (entries[i]->objectName().startsWith("workspace.")) last = i;
      m_viewMenu->insertAction(last >= 0 ? entries.value(last + 1) : nullptr, a);
    }
    a->setCheckable(true);
    if (a->actionGroup() != m_workspaceGroup) m_workspaceGroup->addAction(a);
  }
  m_sketchWorkspace = layout.index("sketch");
  // The last one by id (ui/workspaceId). ui/workspace keeps Review as 0 and Design as 1 for earlier builds, which read it as
  // a number (a build in between wrote the id there), and wins when one of them switched since. A contextual or missing
  // one (an area that is off): Review.
  const QString legacy = m_settings.value("ui/workspace").toString();
  QString saved = m_settings.value("ui/workspaceId", legacy).toString();
  if (legacy == "1") saved = "design";
  else if (legacy == "0" && saved == "design") saved = "review";
  const int restored = layout.index(saved);
  m_ribbon->setWorkspace(restored >= 0 && !layout.spaces[restored].workspace.contextual ? restored : layout.index("review"));
  m_workspaceId = m_workspaceIds.value(m_ribbon->workspace());
  action("workspace." + m_workspaceId)->setChecked(true);
  connect(m_ribbon, &RibbonBar::workspaceChanged, this, [this](int i) {  // from the shortcuts or the chip's list
    if (i != m_sketchWorkspace && m_design && m_design->sketchActive()) return m_ribbon->setWorkspace(m_sketchWorkspace);  // a sketch is open: finish it first
    const QString id = m_workspaceIds.value(i);
    if (std::exchange(m_workspaceId, id) != id) forEachArea([&id](AreaController* area) { area->workspaceChanged(id); });
    updateCommands();
    if (m_ribbon->workspaceAt(i).contextual) return;  // entered and left with the sketch (or an area's mode), never remembered
    m_settings.setValue("ui/workspace", id == "design" ? 1 : 0);
    m_settings.setValue("ui/workspaceId", id);
    if (QAction* a = action("workspace." + id)) a->setChecked(true);
    if (id == "design" && m_doc->hasDocument && m_viewport->selectionFilter() != Viewport::SelFilter::Body) action("select.bodies")->trigger();  // Design works on bodies
    const QString keys = workspaceKeys();
    statusBar()->showMessage(keys.isEmpty() ? tr("%1 workspace").arg(m_ribbon->workspaceAt(i).name)
                                            : tr("%1 workspace · %2 switch workspace").arg(m_ribbon->workspaceAt(i).name, keys), 4000);
  });
  // The compact Select control: the filters as icons with their keys, the rest of selecting under "Select ▾".
  auto* selectMore = new QMenu(this);
  selectMore->addActions(acts({"edit.selectall", "edit.invert", "select.through", "select.similar", "edit.selectparent"}));
  // Each filter's key as bound now (keys::spec of its command id), following a change in the shortcut editor.
  m_ribbon->setSelectFilters(acts({"select.bodies", "select.faces", "select.edges", "select.vertices"}), {"select.bodies", "select.faces", "select.edges", "select.vertices"}, selectMore);
  // The tab row's cluster: quick access (Save, Undo ▾, Redo ▾), search, the areas' widgets, settings.
  m_ribbon->addQuickAction(action("file.save"));
  m_ribbon->addQuickAction(action("edit.undo"), historyMenu(true));
  m_ribbon->addQuickAction(action("edit.redo"), historyMenu(false));
  m_ribbon->setSearchAction(action("tools.commands"));
  QAction* settingsAction = addAction("tools.settings", tr("Settings"), "settings", QKeySequence(), [] {});
  // The gear menu: Preferences (UI-110) for everything that is set once, then the switches used every day.
  auto* settings = new QMenu(this);
  settings->addAction(action("tools.preferences"));
  settings->addAction(action("tools.shortcuts"));
  settings->addSeparator();
  settings->addAction(action("view.dark"));
  QMenu* navMenu = settings->addMenu(tr("Navigation preset"));
  for (QAction* a : m_actions) if (a->objectName().startsWith("nav.")) navMenu->addAction(a);
  QMenu* panels = settings->addMenu(tr("Panels"));
  panels->addActions(acts({"panel.browser", "panel.annotations", "panel.timeline", "panel.reset"}));
  settings->addSeparator();
  settings->addAction(tr("Recover documents"),this,[this]{m_recovery->offerRecovery();});
  settings->addAction(tr("Agent activity"),this,[this]{m_agent->showActivity();});
  settings->addSeparator();
  legal::applySettings();  // the ODA File Converter is opt-in (its terms: non-members non-commercial only)
  CommandInfo odaInfo;
  odaInfo.id = "files.useOda";
  odaInfo.label = tr("Use the ODA File Converter for DWG");
  odaInfo.checkable = true;
  odaInfo.group = commands::defaultGroup("file");
  odaInfo.keywords = {"DWG", "ODA", "converter", "LibreDWG"};
  auto* oda = addCommand(odaInfo, [] {});
  oda->setChecked(m_settings.value("files/useOda", false).toBool());
  connect(oda, &QAction::toggled, this, [this, oda](bool on) { legal::setUseOda(this, oda, on); });
  // The ODA switch, KiCad boards and the view cube's edges and corners are set once: Preferences (Files; Keyboard and mouse),
  // so the gear menu keeps its few everyday entries (UI-110).
  m_ribbon->setSettingsMenu(settingsAction, settings);
  auto* host = new QToolBar(tr("Ribbon"), this);
  host->setObjectName("ribbonHost");
  host->setMovable(false);
  host->setFloatable(false);
  host->setContentsMargins(0, 0, 0, 0);
  host->addWidget(m_ribbon);
  addToolBar(Qt::TopToolBarArea, host);
}
