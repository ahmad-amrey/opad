#pragma once
// The documentation area (TODO 11, track t5a: drawings, part properties, bills of materials, 2D export) as a feature area
// (AreaController.hpp): File > Export bill of materials and Inspect > Part properties with their menu, ribbon and context
// menu places, the browser's Drawings folder (DrawingsFolder.hpp: rename in place, Del, the rows' menu with Export
// sheet… / Export drawing…), the Properties panel's PART section with its link, and the dialogs and jobs they start.
// The Drawings workspace (Ctrl+3, DocsWorkspace.cpp, UI-78): its ribbon tab (temporary until the ribbon is reorganised),
// the sheet page in the viewport's place (SheetPage, SheetCanvas), New drawing from a template, new sheets, sheet
// properties, a template from a DXF or DWG file, base, projected and isometric views placed with the mouse, view styles.
#include <functional>
#include <string>
#include <vector>

#include "AreaController.hpp"
#include "opad/json.hpp"

class SheetPage;
class QMenu;

class DocsArea : public AreaController {
  Q_OBJECT
 public:
  using AreaController::AreaController;
  void buildActions() override;
  void menus(QMenuBar* bar, const QMap<QString, QMenu*>& menus) override;
  void ribbon(RibbonLayout& layout) override;
  void ready() override;
  void contextMenu(const SelectionContext& selection, QMenu& menu) override;
  void selectionChanged(const SelectionContext& selection) override;
  void documentChanged(bool replaced) override;
  void workspaceChanged(const QString& id) override;

  // From the PART section's link, the context menu or Inspect > Part properties: the bodies and components of `ids`.
  void editPartProperties(std::vector<std::string> ids);
  void exportBom(std::vector<std::string> ids = {});  // File > Export bill of materials: the dialog, then where to
  // A sheet as PDF, SVG, DXF, DWG or PNG, a drawing ("drawing:<name>") as the PDF pages of its sheets (UI-86): the file's
  // type is the format, the views projected on a worker (ExportJob.hpp); OPAD_BENCH_EXPORT_OUT skips the file dialog.
  void exportSheet(const std::string& id);
  void rowMenu(const std::string& id, QMenu& menu);  // the Drawings folder's menu of a row
  opad::json lastExport;  // the last sheet export's result, or {"error"} (benches)
  static DocsArea* of(const std::vector<AreaController*>& areas);  // the window's one (benches)

  // The Drawings workspace (DocsWorkspace.cpp).
  void newDrawing(std::vector<std::string> nodes = {});  // the dialog; the views measured on a worker; one `sheet` step
  void createDrawing(const opad::json& args, std::function<void(const std::string& sheet)> done = {});  // without the dialog
  void newSheet();         // one more sheet in the shown sheet's drawing, on its paper and template
  void sheetProperties();  // the dialog, then one sheet_edit
  void templateFromFile(const QString& file = {});  // read on a worker; the sheet takes its frame, title block and paper
  void placeView(const std::string& orient);  // a base view placed with the mouse (front, top, ..., iso)
  void placeProjected();                      // a view projected from the selected one, placed with the mouse
  void setViewStyle(const std::vector<std::string>& views, const opad::json& style);  // merged into each view's style
  void openSheet(const std::string& rowId);   // a Drawings folder row: its sheet in the Drawings workspace
  void viewMenu(const std::vector<std::string>& views, QMenu& menu);  // right-click on views
  // A command once nothing reads the document (the sheet's own worker is stopped for it); `then` gets the result, or null
  // when it was refused (a message box said why).
  void run(const std::string& command, opad::json args, std::function<void(const opad::json&)> then = {});
  void whenFree(std::function<void()> fn);
  SheetPage* sheetPage() const { return m_page; }

 private:
  void buildDrawingCommands();
  void drawingsRibbon(RibbonLayout& layout);
  void readyDrawings();
  void syncStyleActions();  // Hidden lines / Tangent edges checked as the selected views show them
  SheetPage* m_page = nullptr;
};
