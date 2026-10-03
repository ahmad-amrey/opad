#pragma once
// Feature areas (assembly, assets, drawings, version control, ...) plug into the main window through this interface
// instead of editing MainWindow (UI-119): an area is one class in its own file, registered by one line after it,
//   class Assets : public AreaController { ... };
//   OPAD_AREA(Assets)
// MainWindow creates every registered area once, in name order, right after its own commands are built; it owns them
// and deletes them first when it goes. It calls the hooks below at the matching points (the defaults do nothing):
//   construction, in this order: buildActions, menus, ribbon, statusWidgets, ready;
//   then, once ready: contextMenu, selectionChanged, positionOverlays, documentChanged, maybeClose.
// What an area needs of the window comes through services(); its own state stays in the area. Browser rows and the
// Properties panel take providers (BrowserPanel::addDecorator / addFolder, PropertiesPanel::addSectionProvider),
// registered in ready(). Hooks run on the UI thread: anything that scales with the model goes through jobs().
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
class ToolPanel;
class Viewport;
struct RibbonLayout;

// The selection as the hooks see it: what the viewport or the browser reported last.
struct SelectionContext {
  std::vector<std::string> ids;  // nodes (bodies, components), sketches or browser folder rows, each once, in order
  std::vector<opad::Ref> refs;   // as picked in the view (faces, edges, ...); from the browser one body ref per id
  bool sketching = false;        // a sketch is open: the context menu is the sketch's
  bool empty() const { return ids.empty() && refs.empty(); }
};

// The window's services for areas. The window is built around the construction hooks, so a pointer can still be null
// there (buildActions and menus come first; the viewport is there from ribbon on, the browser, the Properties panel and
// jobs from statusWidgets on, the design controller only in ready); from ready() on everything is there.
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
  QAction* action(const QString& id) const;  // any command by id: "file.open", "view.fit", an area's own; null if none
  // A command like the built-in ones: its shortcut from the user's settings, locked while a file loads, errors shown as
  // a message box; in viewer mode it asks to save as OPAD first when MainWindow::isEditAction(id) says it edits.
  QAction* addAction(const QString& id, const QString& text, const QString& icon, const QKeySequence& shortcut, std::function<void()> fn,
                     bool checkable = false);
  void addPanel(ToolPanel* panel);   // a floating panel of the window: anchored to the viewport, closed by openPanel and Esc
  void openPanel(ToolPanel* panel);  // shows it over the viewport; the other unpinned panels close
  bool requireEditable(std::function<void()> resume = {});  // viewer mode: offers to save as OPAD first; false until then
  void guarded(const std::function<void()>& fn);           // runs fn; an exception becomes a message box
  void showMessage(const QString& text, int ms = 4000);    // status bar
  SelectionContext selection() const;                       // the current one
  void positionOverlays();  // lay the overlays out again (the areas' positionOverlays too)
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
  // Before the document is replaced (open, new, close) or the window closes: false keeps it, e.g. when the user cancels
  // giving up unfinished work. Asked first, also in benches (which answer no other question).
  virtual bool maybeClose() { return true; }

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
