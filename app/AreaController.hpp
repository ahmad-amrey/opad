#pragma once
// Feature areas (assembly, assets, drawings, version control, ...) plug into the main window through this interface
// instead of editing MainWindow (UI-119): an area is one class in its own file, registered by one line after it,
//   class Assets : public AreaController { ... };
//   OPAD_AREA(Assets)
// MainWindow creates every registered area once, in name order, right after its own commands are built; it owns them
// and deletes them first when it goes. It calls the hooks below at the matching points (the defaults do nothing):
//   construction, in this order: buildActions, menus, ribbon, statusWidgets, ready;
//   then, once ready: contextMenu, selectionChanged, positionOverlays, documentChanged, workspaceChanged, maybeClose, command.
// What an area needs of the window comes through services(); its own state stays in the area. Browser rows and the
// Properties panel take providers (BrowserPanel::addDecorator / addFolder, PropertiesPanel::addSectionProvider), the
// chips row takes chips (ViewportChips::addChip), all registered in ready(); a workspace of its own is a RibbonLayout
// entry in ribbon(), and the ribbon's tab row takes widgets (AreaServices::addTabRowWidget, e.g. a branch chip). Hooks
// run on the UI thread: anything that scales with the model goes through jobs().
#include <QMap>
#include <QObject>
#include <QRect>
#include <QString>
#include <QStringList>
#include <functional>
#include <string>
#include <vector>

#include "opad/util.hpp"

class AppDocument;
class BrowserPanel;
class DesignController;
class JobRunner;
class MainWindow;
class PropertiesPanel;
class QAction;
class QKeySequence;
class QMainWindow;
class QMenu;
class QMenuBar;
class QStatusBar;
class QWidget;
class TimelineWidget;
class ToolPanel;
class Viewport;
class ViewportChips;
class CommandRegistry;
struct CommandInfo;
struct RibbonLayout;

// The selection as the hooks see it: what the viewport or the browser reported last.
struct SelectionContext {
  std::vector<std::string> ids;  // nodes (bodies, components), sketches or browser folder rows, each once, in order
  std::vector<opad::Ref> refs;   // as picked in the view (faces, edges, ...); from the browser one body ref per id
  bool sketching = false;        // a sketch is open: the context menu is the sketch's
  bool empty() const { return ids.empty() && refs.empty(); }
};

// The window's services for areas. The window is built around the construction hooks, so a pointer can still be null
// there (buildActions and menus come first; the viewport and its chips are there from ribbon on, the browser, the
// Properties panel and jobs from statusWidgets on, the design controller only in ready); from ready() on everything is there.
class AreaServices {
 public:
  explicit AreaServices(MainWindow* window) : m_window(window) {}
  QMainWindow* window() const;          // parent for dialogs and ToolPanels
  AppDocument* document() const;
  Viewport* viewport() const;
  JobRunner* jobs() const;              // every long operation (see Jobs.hpp)
  DesignController* design() const;
  BrowserPanel* browser() const;        // row decorations and folders
  PropertiesPanel* properties() const;  // property sections
  TimelineWidget* timeline() const;     // the op markers (setCurrentOp, pulse)
  ViewportChips* chips() const;         // the chips row over the viewport (addChip)
  // A widget in the ribbon's tab row (a branch chip): in the cluster after search, before settings; from ribbon on.
  void addTabRowWidget(QWidget* widget);
  QAction* action(const QString& id) const;  // any command by id: "file.open", "view.fit", an area's own; null if none
  // A command like the built-in ones: its shortcut from the user's settings, locked while a file loads, errors shown as
  // a message box; in viewer mode it asks to save as OPAD first when its record says it edits the document. addCommand
  // takes the whole record (Commands.hpp: group, keywords, help id, workspaces, editsDocument, enabledWhen); addAction
  // makes one from the id as the built-in commands do (editsDocument by MainWindow::isEditAction: false for an area's id).
  QAction* addCommand(const CommandInfo& info, std::function<void()> fn);
  QAction* addAction(const QString& id, const QString& text, const QString& icon, const QKeySequence& shortcut, std::function<void()> fn,
                     bool checkable = false);
  const CommandRegistry& commands() const;  // every command's record, the built-in ones and the areas'
  void updateCommands();                    // re-asks every enabledWhen (it is asked on document, selection and workspace changes)
  void addPanel(ToolPanel* panel);   // a floating panel of the window: anchored to the viewport, closed by openPanel and Esc
  void openPanel(ToolPanel* panel);  // shows it over the viewport; the other unpinned panels close
  bool requireEditable(std::function<void()> resume = {});  // viewer mode: offers to save as OPAD first; false until then
  void guarded(const std::function<void()>& fn);           // runs fn; an exception becomes a message box
  void showMessage(const QString& text, int ms = 4000);    // status bar
  // A toast at the bottom centre of the viewport (Toast.hpp): a result or a warning, with an optional action ("Undo")
  // whose callback runs when it is clicked; ms 0 keeps it until it is closed. From ribbon on.
  void toast(const QString& text, const QString& actionText = QString(), std::function<void()> callback = {}, int ms = 4000);
  // The toast of a change just made: its Undo takes back the document's last step, unless another came after it.
  void undoToast(const QString& text);
  SelectionContext selection() const;                       // the current one
  // Makes these the selection as if they were picked and tells the window and the areas (selectionChanged): faces, edges
  // and vertices in the view (its filter must be picking them), bodies and components as from the browser.
  void select(const std::vector<opad::Ref>& refs);
  void positionOverlays();  // lay the overlays out again (the areas' positionOverlays too)
  // The workspace shown, by RibbonLayout id: "review", "design", "sketch" (contextual, while a sketch is open) or an
  // area's; from statusWidgets on. setWorkspace("drawings") is what its command "workspace.drawings" does: an unknown id or
  // "sketch" changes nothing, and while a sketch is open the ribbon stays on it. An area's contextual workspace is
  // entered and left this way too (it is not remembered at exit).
  QString workspace() const;
  void setWorkspace(const QString& id);
  // Shows a contextual tab the area added (RibbonLayout::addContextualTab) first in its workspace's row, current, in its
  // accent; hidden again, the tab that was current comes back. False: no such contextual tab.
  bool setContextualTab(const QString& id, bool shown);
 private:
  MainWindow* m_window;
};

class AreaController : public QObject {
  Q_OBJECT
 public:
  explicit AreaController(AreaServices& services) : m_services(services) {}
  AreaServices& services() const { return m_services; }

  // Construction.
  virtual void buildActions() {}  // add commands (services().addAction); after the built-in ones, in area order
  // Add entries to the menu bar's menus ("file", "edit", "view", "inspect", "design", "tools", "help"), or menus of its own.
  virtual void menus(QMenuBar* bar, const QMap<QString, QMenu*>& menus) {}
  virtual void ribbon(RibbonLayout& layout) {}  // add workspaces, tabs or groups before the ribbon is built (Ribbon.hpp)
  virtual void statusWidgets(QStatusBar* bar) {}  // after the built-in widgets (addPermanentWidget keeps them on the right)
  virtual void ready() {}  // the window is built: panels, browser decorations and folders, property sections, connections

  // From ready() on.
  virtual void contextMenu(const SelectionContext& selection, QMenu& menu) {}  // right-click in the view or browser: add at the end
  virtual void selectionChanged(const SelectionContext& selection) {}  // the picks of a feature input are no selection
  virtual void positionOverlays(const QRect& viewport) {}  // the viewport (global) moved or resized: place what floats over it
  // After every change of the document (edit, undo, load, close); replaced: it is another document, or none.
  virtual void documentChanged(bool replaced) {}
  // Another workspace is shown (services().workspace()): the user switched, or a sketch opened ("sketch") or closed. The
  // one the window starts in is not reported: read it in ready(). An area's own workspace swaps its panels or pages here.
  virtual void workspaceChanged(const QString& id) {}
  // Before the document is replaced (open, new, close) or the window closes: false keeps it, e.g. when the user cancels
  // giving up unfinished work. Asked first, also in benches (which answer no other question).
  virtual bool maybeClose() { return true; }
  // A built-in command about to act on the selection: "edit.delete" (Del) and "edit.selectparent" (Ctrl+Up). True: the
  // area did it, the window does nothing more (smart selection takes Del on picked faces and grows them to their
  // feature). Asked in area order; the first true wins.
  virtual bool command(const QString& id, const SelectionContext& selection) { return false; }

 private:
  AreaServices& m_services;
};

namespace areas {
using Factory = AreaController* (*)(AreaServices& services);  // may return null: the area is off in this run
bool add(const char* name, Factory factory);                  // static registration (OPAD_AREA)
QStringList names();                                          // registered, sorted
QStringList clashes();                                        // names registered twice (the first one keeps it)
std::vector<AreaController*> create(AreaServices& services);  // one of each, by name; the caller owns them
}  // namespace areas

#define OPAD_AREA(Class) \
  [[maybe_unused]] static const bool Class##_area_added = areas::add(#Class, [](AreaServices& s) -> AreaController* { return new Class(s); });
