// Feature areas (AreaController.hpp): what they reach of the window, and their creation.
#include "MainWindow.hpp"

#include <QStatusBar>

#include <set>

void MainWindow::createAreas() {
  m_areas = areas::create(m_areaServices);
  for (AreaController* area : m_areas) area->setParent(this);
  for (AreaController* area : m_areas) area->buildActions();
}

SelectionContext MainWindow::selectionContext() const {
  SelectionContext s;
  s.refs = m_selRefs;
  std::set<std::string> seen;
  for (const auto& r : m_selRefs)
    if (r.kind != opad::Ref::Kind::Point && seen.insert(r.body).second) s.ids.push_back(r.body);
  s.sketching = m_design && m_design->sketchActive();
  return s;
}

QMainWindow* AreaServices::window() const { return m_window; }
AppDocument* AreaServices::document() const { return m_window->m_doc; }
Viewport* AreaServices::viewport() const { return m_window->m_viewport; }
JobRunner* AreaServices::jobs() const { return m_window->m_jobs; }
DesignController* AreaServices::design() const { return m_window->m_design; }
BrowserPanel* AreaServices::browser() const { return m_window->m_browser; }
PropertiesPanel* AreaServices::properties() const { return m_window->m_props; }
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
void AreaServices::guarded(const std::function<void()>& fn) { m_window->guarded(fn); }
void AreaServices::showMessage(const QString& text, int ms) { m_window->statusBar()->showMessage(text, ms); }
SelectionContext AreaServices::selection() const { return m_window->selectionContext(); }
void AreaServices::positionOverlays() { m_window->positionOverlays(); }
QString AreaServices::workspace() const { return m_window->workspaceId(); }
void AreaServices::setWorkspace(const QString& id) { m_window->setWorkspace(id); }
