// Menus, the ribbon (workspaces, tabs, its settings menu) and the Tools and Help commands.
#include "MainWindow.hpp"
#include "AgentBridge.hpp"
#include "FileAssociations.hpp"
#include "Legal.hpp"
#include "RecoveryManager.hpp"

#include <QActionGroup>
#include <QCursor>
#include <QDir>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QToolBar>

#include <algorithm>
#include <utility>

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
  // The Tools menu's own entries (Appendix A §4) and the gear menu's: what used to be plain entries of one menu.
  addAction("tools.ai", tr("AI integration…"), "agent", QKeySequence(), [this] { if (m_agent) m_agent->settings(); });
  addAction("tools.agentActivity", tr("Agent activity"), "history", QKeySequence(), [this] { if (m_agent) m_agent->showActivity(); });
  // The files OPAD opens from Explorer (Preferences > Files has the same button); where the system declares them, the page.
  addAction("tools.fileTypes", tr("File types…"), "open", QKeySequence(), [this] {
    if (associations::supported()) FileTypesDialog(this).exec();
    else PreferencesDialog::open(this, "files");
  });
  addAction("file.recover", tr("Recover documents…"), "restore", QKeySequence(), [this] { if (m_recovery) m_recovery->offerRecovery(); });
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

// The menu bar (UI-104, Appendix A §4): File, Edit, View, Insert, Inspect, Design, Sketch (while sketching), Version, Tools,
// Help. Edit keeps undoing, selecting and the object's name and history; how things show is View's (Hide, Show notes),
// notes are Inspect's beside the measuring, inserts have their own menu and the selection filters sit in the ribbon's
// Select control with their keys 1 to 4. The areas add theirs (AreaController::menus).
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
  add(file, {"-", "file.close", "-", "file.save", "file.saveas", "file.savetemplate", "-", "file.export", "file.screenshot", "-", "file.recover", "-", "file.quit"});
  QMenu* edit = menuBar()->addMenu(tr("&Edit"));
  add(edit, {"edit.undo", "edit.redo", "edit.repeat", "-", "edit.selectall", "edit.invert", "edit.selectparent", "edit.selecttouched", "-", "edit.rename", "edit.delete",
             "edit.restore", "-", "edit.filter"});
  QMenu* view = m_viewMenu = menuBar()->addMenu(tr("&View"));
  add(view, {"view.fit", "view.fitall", "view.home", "view.rollleft", "view.rollright", "-", "view.top", "view.front", "view.right", "view.iso", "view.bottom", "view.back", "view.left", "-", "view.ortho", "view.shaded", "view.edges", "view.wire", "view.hidden", "view.hiddenEdges", "view.grid", "view.gridSettings", "select.through", "-", "view.isolate", "view.unisolate", "view.hideothers", "edit.hide", "edit.showall", "annotate.show", "-", "view.saveview"});
  m_viewsMenu = view->addMenu(tr("Named views"));
  m_viewsMenu->setObjectName("views");
  view->addSeparator();
  QMenu* nav = view->addMenu(tr("Navigation preset"));
  nav->setObjectName("navigation");
  add(nav, {"nav.fusion", "nav.solidworks", "nav.onshape", "nav.blender"});
  m_snappingMenu = view->addMenu(tr("Snapping"));  // the status bar's switches join Object snap (buildStatusBar)
  m_snappingMenu->setObjectName("snapping");
  add(m_snappingMenu, {"drawing2d.objectSnap"});
  add(view, {"view.dark", "-", "workspace.review", "workspace.design", "-", "panel.browser", "panel.annotations", "panel.section", "panel.timeline", "panel.reset", "-",
             "view.nextRegion", "view.previousRegion"});
  QMenu* insert = menuBar()->addMenu(tr("I&nsert"));  // the areas add theirs before the separator (one click), submenus after it
  insert->setObjectName("insertMenu");
  add(insert, {"file.import", "-"});
  QMenu* inspect = menuBar()->addMenu(tr("&Inspect"));
  add(inspect, {"inspect.distance", "inspect.angle", "inspect.radius", "inspect.bbox", "inspect.length", "inspect.pin", "inspect.clear", "-", "inspect.properties", "select.similar", "-", "inspect.interference", "inspect.printcheck", "-", "inspect.section", "inspect.flip", "-", "annotate.add", "annotate.draw", "annotate.resolve"});
  QMenu* designMenu = menuBar()->addMenu(tr("&Design"));
  add(designMenu, {"design.sketch", "design.convertDrawing", "design.parameters", "-"});
  for (const char* group : {"create", "modify", "combine", "pattern", "body", "construct"}) {
    QMenu* sub = designMenu->addMenu(i18n::t(QString(group).left(1).toUpper() + QString(group).mid(1)));
    sub->setObjectName(group);
    for (const auto& spec : opad::design::feature_specs())  // the stored interference check: Inspect > Interference's Keep as check
      if (spec.group == group && spec.kind != "interference") sub->addAction(action("design." + QString::fromStdString(spec.kind)));
    if (QString(group) == "construct") add(sub, {"-", "design.showOrigin"});
  }
  add(designMenu, {"-", "design.edit", "design.suppress", "timeline.rollBack", "design.regenerate", "-", "design.newcomponent", "design.reparent", "design.colour", "design.opacity", "design.lock"});
  // Sketch: while a sketch is open (updateDesignState), every tool by its group and the sketch's own commands.
  m_sketchMenu = menuBar()->addMenu(tr("&Sketch"));
  m_sketchMenu->setObjectName("sketchMenu");
  add(m_sketchMenu, {"sketch.moreCreate", "sketch.moreModify", "sketch.moreConstrain", "sketch.moreReference", "sketch.moreFiles", "-", "sketch.select",
                     "sketch.dimension", "sketch.construction", "sketch.node", "sketch.copybase", "-", "sketch.showConstraints", "sketch.openEnds", "sketch.constraints",
                     "sketch.snaps", "sketch.selectionOptions", "sketch.panel", "sketch.commandLine", "sketch.replane", "-", "sketch.cancel", "sketch.finish"});
  m_sketchMenu->menuAction()->setVisible(false);
  QMenu* version = menuBar()->addMenu(tr("Ve&rsion"));  // version control's (VcsArea): hidden when no area fills it
  version->setObjectName("versionMenu");
  QMenu* tools = menuBar()->addMenu(tr("&Tools"));
  add(tools, {"tools.commands", "tools.shortcuts", "-", "tools.ai", "tools.agentActivity", "-", "tools.fileTypes", "tools.cache"});
  QMenu* help = menuBar()->addMenu(tr("&Help"));
  add(help, {"help.licenses", "help.aboutqt", "-", "help.about"});
  const QMap<QString, QMenu*> menus{{"file", file},       {"edit", edit},         {"view", view},       {"insert", insert}, {"inspect", inspect},
                                    {"design", designMenu}, {"sketch", m_sketchMenu}, {"version", version}, {"tools", tools},   {"help", help}};
  for (AreaController* area : m_areas) area->menus(menuBar(), menus);
  version->menuAction()->setVisible(!version->isEmpty());
  if (QAction* last = insert->actions().value(insert->actions().size() - 1); last && last->isSeparator()) insert->removeAction(last);  // no submenus after it
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

void MainWindow::setWorkspace(const QString& id) {
  if (const int i = m_workspaceIds.indexOf(id); i >= 0) m_ribbon->setWorkspace(i);
}

bool MainWindow::switchesToDesign(const QString& id) const {
  if (!id.startsWith("design.") || m_workspaceId == "design" || !m_workspaceIds.contains("design")) return false;
  if (id == "design.colour" || id == "design.opacity" || id == "design.lock") return false;  // how it looks: a view setting where it is
  const CommandInfo* c = m_commands.find(id);
  return c && c->workspaces.contains("design") && !c->workspaces.contains(m_workspaceId);
}

void MainWindow::followDrawing(bool drawing) {
  if (drawing) {
    if (m_workspaceId == "drafting" || !m_workspaceIds.contains("drafting") || (m_design && m_design->sketchActive())) return;
    m_workspaceBeforeDrafting = m_workspaceId;
    setWorkspace("drafting");
  } else if (m_workspaceId == "drafting" && !m_workspaceBeforeDrafting.isEmpty()) {
    setWorkspace(std::exchange(m_workspaceBeforeDrafting, QString()));
  }
}

QAction* MainWindow::menuCommand(const CommandInfo& info, QMenu* menu) {
  QAction* a = addCommand(info, [menu] { menu->popup(QCursor::pos()); });
  a->setMenu(menu);
  return a;
}

void MainWindow::buildRibbonMenus() {
  // Named views ▾: the View menu's list (the same entries: an exploded view explodes again, ExplodeArea), then Save view….
  auto* views = new QMenu(this);
  views->setObjectName("ribbonNamedViews");
  connect(views, &QMenu::aboutToShow, this, [this, views] {
    views->clear();
    views->addActions(m_viewsMenu->actions());
    views->addSeparator();
    views->addAction(action("view.saveview"));
  });
  connect(views, &QMenu::triggered, m_viewsMenu, &QMenu::triggered);
  const CommandInfo named{"view.namedViews", tr("Named views"), "recent"};
  menuCommand(named, views);
  // Rendering ▾: how the view draws while it moves and in which theme; Preferences > Display has the rest.
  auto* rendering = new QMenu(this);
  rendering->setObjectName("ribbonRendering");
  for (const char* id : {"view.hideSmallParts", "view.smallPartSize", "view.adaptive", "view.animate", "-", "view.dark"})
    if (QString(id) == "-") rendering->addSeparator();
    else if (QAction* a = action(id)) rendering->addAction(a);
  const CommandInfo look{"view.rendering", tr("Rendering"), "shaded"};
  menuCommand(look, rendering);
  // Panels ▾: the window's panels by their keys, and the layout as it came.
  auto* panels = new QMenu(this);
  panels->setObjectName("ribbonPanels");
  for (const char* id : {"panel.browser", "panel.annotations", "panel.timeline", "vcs.panel", "panel.section", "-", "panel.reset"})
    if (QString(id) == "-") panels->addSeparator();
    else if (QAction* a = action(id)) panels->addAction(a);
  const CommandInfo shown{"view.panels", tr("Panels"), "list"};
  menuCommand(shown, panels);
}

void MainWindow::buildRibbon() {
  buildRibbonMenus();
  m_ribbon = new RibbonBar(this);
  auto acts = [&](std::initializer_list<const char*> ids) {
    QList<QAction*> out;
    for (const char* id : ids) if (QAction* a = action(id)) out << a;
    return out;
  };
  RibbonLayout layout;
  ribbonTable(layout);
  for (AreaController* area : m_areas) area->ribbon(layout);  // their own workspaces (Drawings) and contextual tabs
  draftingTable(layout);
  for (RibbonLayout::Space& space : layout.spaces)  // a tab whose commands this build does not have is not shown
    space.tabs.removeIf([](const RibbonLayout::Tab& tab) {
      return std::none_of(tab.groups.begin(), tab.groups.end(), [](const RibbonLayout::Group& g) {
        return std::any_of(g.items.begin(), g.items.end(), [](const RibbonLayout::Item& i) { return i.action != nullptr; });
      });
    });
  QString modifiers;  // of the first key: "Ctrl+1 / 2 / Ctrl+Alt+D"
  for (const RibbonLayout::Space& space : layout.spaces) {
    const int index = m_ribbon->addWorkspace(space.workspace);
    m_workspaceIds << space.id;
    for (const RibbonLayout::Tab& tab : space.tabs) {
      for (const RibbonLayout::Group& group : tab.groups)
        for (const RibbonLayout::Item& item : group.items)
          for (QAction* a : QList<QAction*>{item.action} + item.variants + (item.action && item.action->menu() ? item.action->menu()->actions() : QList<QAction*>()))
            if (a && !a->isSeparator() && !a->objectName().isEmpty()) m_commands.addWorkspace(a->objectName(), space.id);  // a dropdown's entries too
      m_ribbon->addTab(index, tab);
    }
    if (space.workspace.contextual) continue;
    const QString key = space.workspace.key;
    if (m_workspaceKeys.isEmpty()) {
      m_workspaceKeys = key;
      modifiers = key.left(key.lastIndexOf('+') + 1);
    } else if (!key.isEmpty()) {
      const bool same = !modifiers.isEmpty() && key.startsWith(modifiers) && key.lastIndexOf('+') + 1 == modifiers.size();
      m_workspaceKeys += " / " + (same ? key.mid(modifiers.size()) : key);
    }
    QAction* a = action("workspace." + space.id);
    if (!a) {  // an area's workspace: its command beside the others in the View menu
      a = addAction("workspace." + space.id, tr("%1 workspace").arg(space.workspace.name), space.workspace.icon, QKeySequence(key), [this, id = space.id] { setWorkspace(id); }, true);
      const QList<QAction*> entries = m_viewMenu->actions();
      int last = -1;
      for (int i = 0; i < entries.size(); ++i)
        if (entries[i]->objectName().startsWith("workspace.")) last = i;
      m_viewMenu->insertAction(last >= 0 ? entries.value(last + 1) : nullptr, a);
      m_commands.setMenuPath(a->objectName(), "view");
    }
    a->setCheckable(true);
    if (a->actionGroup() != m_workspaceGroup) m_workspaceGroup->addAction(a);
  }
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
    const QString id = m_workspaceIds.value(i);
    if (id != "drafting") m_workspaceBeforeDrafting.clear();  // left by hand (or by followDrawing): the next document stays where it is
    if (id != "design" && m_design && m_design->sketchActive()) {  // a sketch is open: its tab stays until it is finished
      statusBar()->showMessage(tr("Finish or cancel the sketch first"), 4000);
      return setWorkspace("design");
    }
    if (std::exchange(m_workspaceId, id) != id) forEachArea([&id](AreaController* area) { area->workspaceChanged(id); });
    updateCommands();
    if (m_ribbon->workspaceAt(i).contextual) return;  // entered and left with the sketch (or an area's mode), never remembered
    m_settings.setValue("ui/workspace", id == "design" ? 1 : 0);
    m_settings.setValue("ui/workspaceId", id);
    if (QAction* a = action("workspace." + id)) a->setChecked(true);
    if (id == "design" && m_doc->hasDocument && !(m_design && m_design->sketchActive()) && m_viewport->selectionFilter() != Viewport::SelFilter::Body)
      action("select.bodies")->trigger();  // Design works on bodies
    statusBar()->showMessage(tr("%1 workspace · %2 switch workspace").arg(m_ribbon->workspaceAt(i).name, m_workspaceKeys), 4000);
  });
  // The compact Select control: the filters as icons with their keys, the rest of selecting under "Select ▾".
  auto* selectMore = new QMenu(this);
  selectMore->addActions(acts({"edit.selectall", "edit.invert", "select.through", "select.similar", "edit.selectparent"}));
  const QList<QAction*> filters = acts({"select.bodies", "select.faces", "select.edges", "select.vertices"});
  m_ribbon->setSelectFilters(filters, {"1", "2", "3", "4"}, selectMore);
  for (const QString& ws : m_workspaceIds)  // the control ends every workspace's strip: the filters' place (they left the Edit menu)
    for (QAction* a : filters + selectMore->actions()) m_commands.addWorkspace(a->objectName(), ws);
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
  settings->addActions(acts({"file.recover", "tools.agentActivity"}));
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
