// Feature areas (AreaController.hpp): what they reach of the window, and their creation.
#include "MainWindow.hpp"
#include "CheckPanel.hpp"
#include "DrawingPlacer.hpp"

#include <QStatusBar>

#include <algorithm>
#include "StatusRow.hpp"

#include <set>

void MainWindow::createAreas() {
  m_areas = areas::create(m_areaServices);
  for (AreaController* area : m_areas) area->setParent(this);
  m_ownCommands = m_actions.size();
  for (AreaController* area : m_areas) area->buildActions();
}

SelectionContext MainWindow::selectionContext() const {
  SelectionContext s;
  s.refs = m_selRefs;
  std::set<std::string> seen;
  for (const auto& r : m_selRefs)
    if (r.kind != opad::Ref::Kind::Point && seen.insert(r.body).second) s.ids.push_back(r.body);
  for (const auto& id : m_selRows)
    if (seen.insert(id).second) s.ids.push_back(id);
  s.sketching = m_design && m_design->sketchActive();
  return s;
}

QMainWindow* AreaServices::window() const { return m_window; }
AppDocument* AreaServices::document() const { return m_window->m_doc; }
Viewport* AreaServices::viewport() const { return m_window->m_viewport; }
JobRunner* AreaServices::jobs() const { return m_window->m_jobs; }
DesignController* AreaServices::design() const { return m_window->m_design; }
BrowserPanel* AreaServices::browser() const { return m_window->m_browser; }
void AreaServices::revealBrowser() { m_window->m_browserOverlay->reveal(); }
PropertiesPanel* AreaServices::properties() const { return m_window->m_props; }
TimelineWidget* AreaServices::timeline() const { return m_window->m_timeline; }
void AreaServices::findInTimeline(const std::string& op) const {
  if (!m_window->m_timelineDock->isVisible()) m_window->m_timelineDock->show();
  m_window->m_timeline->setCurrentOp(op);
  m_window->m_timeline->pulse(op);
}
void AreaServices::timelineMenu(QMenu& menu, const std::string& op) const { m_window->buildTimelineMenu(menu, op); }
ViewportChips* AreaServices::chips() const { return m_window->m_chips; }
QAction* AreaServices::action(const QString& id) const { return m_window->action(id); }

QAction* AreaServices::addCommand(const CommandInfo& info, std::function<void()> fn) { return m_window->addCommand(info, std::move(fn)); }
const CommandRegistry& AreaServices::commands() const { return m_window->m_commands; }
void AreaServices::updateCommands() { m_window->updateCommands(); }

QAction* AreaServices::addAction(const QString& id, const QString& text, const QString& icon, const QKeySequence& shortcut, std::function<void()> fn,
                                 bool checkable) {
  return m_window->addAction(id, text, icon, shortcut, std::move(fn), checkable);
}

void AreaServices::addPanel(ToolPanel* panel) {
  if (!m_window->m_panels.contains(panel)) m_window->m_panels << panel;
}

void AreaServices::openPanel(ToolPanel* panel) {
  addPanel(panel);
  m_window->openPanel(panel);
}

bool AreaServices::requireEditable(std::function<void()> resume) { return m_window->requireEditable(std::move(resume)); }
void AreaServices::importFile(const QString& path, bool link) { m_window->importPath(path, link ? 1 : 0); }
void AreaServices::importPlaced(const QString& path, const opad::Mat4& placement, const opad::json& options, bool link, std::function<void()> then) {
  m_window->beginLoad([this, path, then] {
    m_window->addRecent(path);
    m_window->m_viewport->fitWhenReady();
    if (then) then();
  });
  m_window->m_doc->startImport(path, {}, placement, {}, link, options);
}
void AreaServices::guarded(const std::function<void()>& fn) { m_window->guarded(fn); }
void AreaServices::showMessage(const QString& text, int ms) { m_window->statusBar()->showMessage(text, ms); }
void AreaServices::open(const QString& path) { m_window->openPath(path); }
Toast* AreaServices::toast(const QString& text, const QString& actionText, std::function<void()> callback, int ms) {
  return m_window->m_toasts->toast(text, actionText, std::move(callback), ms);
}
Toast* AreaServices::undoToast(const QString& text, int ms) { return m_window->undoToast(text, ms); }
ToastStack* AreaServices::toasts() const { return m_window->m_toasts; }
SelectionContext AreaServices::selection() const { return m_window->selectionContext(); }

void AreaServices::select(const std::vector<opad::Ref>& refs) {
  MainWindow* w = m_window;
  if (std::all_of(refs.begin(), refs.end(), [](const opad::Ref& r) { return r.kind == opad::Ref::Kind::Body; })) {
    std::vector<std::string> ids;
    for (const auto& r : refs)
      if (std::find(ids.begin(), ids.end(), r.body) == ids.end()) ids.push_back(r.body);
    w->m_browser->setSelectedIds(ids);
    return w->onBrowserSelection(ids);
  }
  w->m_viewport->selectRefs(refs);
  w->onViewportSelection();
}

bool MainWindow::areaCommand(const QString& id, const std::string& op) {
  if (!m_areasReady) return false;
  SelectionContext selection = selectionContext();
  selection.op = op;
  for (AreaController* area : m_areas)
    if (area->command(id, selection)) return true;
  return false;
}

QString AreaServices::activeCommand() const {
  const MainWindow& w = *m_window;
  if (!w.m_tool.id.isEmpty()) return w.m_tool.id == "sectionface" ? QString("inspect.section") : "inspect." + w.m_tool.id;
  if (w.m_toolPanel && w.m_toolPanel->isVisible() && w.m_toolStack->currentWidget() == w.m_checks)
    return w.m_checks->mode() == CheckPanel::Mode::Print ? "inspect.printcheck" : "inspect.interference";
  if (w.m_annotationEditor) return w.m_annotationEditor->drawingMode() ? "annotate.draw" : "annotate.add";
  // A plane picked for a sketch, Align view to plane, Import's drawing or Redefine sketch plane (a feature's own plane
  // input is the feature's), and the drawing being placed: Import's.
  if (w.m_design && w.m_design->pickingPlane() && !w.m_design->featureActive()) return w.m_design->planeCommand();
  if (w.m_drawingPlacer && w.m_drawingPlacer->active()) return "file.import";
  if (w.m_design && w.m_design->sketchActive()) return "sketch." + w.m_design->sketch()->tool().replace(':', '.');
  if (w.m_design && w.m_design->featureActive()) return "design." + QString::fromStdString(w.m_design->featurePanel()->spec()->kind);
  return {};
}
void AreaServices::positionOverlays() { m_window->positionOverlays(); }
QString AreaServices::workspace() const { return m_window->workspaceId(); }
void AreaServices::setWorkspace(const QString& id) { m_window->setWorkspace(id); }
bool AreaServices::setContextualTab(const QString& id, bool shown) { return m_window->setContextualTab(id, shown); }
void AreaServices::addTabRowWidget(QWidget* widget) { m_window->m_ribbon->addTabRowWidget(widget); }
void AreaServices::setCentralPage(QWidget* page) {
  if (page && m_window->m_stack->indexOf(page) < 0) m_window->m_stack->addWidget(page);
  m_window->m_centralPage = page;
  m_window->showCentral();
}
QWidget* AreaServices::centralPage() const { return m_window->m_centralPage; }
void AreaServices::addStatusChip(QWidget* chip) { m_window->m_statusRow->addChip(chip); }
PathChip* AreaServices::pathChip() const { return m_window->m_statusPath; }
