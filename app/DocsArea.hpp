#pragma once
// The documentation area (TODO 11, track t5a: drawings, part properties, bills of materials, 2D export) as a feature area
// (AreaController.hpp): File > Export bill of materials and Inspect > Part properties with their menu, ribbon and context
// menu places, the browser's Drawings folder (DrawingsFolder.hpp: rename in place, Del, the rows' menu with Export
// sheet… / Export drawing…), the Properties panel's PART section with its link, and the dialogs and jobs they start.
// The Drawings workspace (Ctrl+3, DocsWorkspace.cpp, UI-78): its ribbon tab (temporary until the ribbon is reorganised),
// the sheet page in the viewport's place (SheetPage, SheetCanvas), New drawing from a template, new sheets, sheet
// properties, a template from a DXF or DWG file, base, projected and isometric views placed with the mouse, view styles.
// Its Annotate tab (DocsAnnotate.cpp, UI-79 to UI-81): dimensions, hole callouts and tables, centre marks and lines, notes,
// datums, feature control frames, surface texture, ordinate/baseline/chain sets and dimensions from datums, re-attach;
// parts lists, balloons (auto-balloon), revision tables and renumbering (UI-84).
#include <functional>
#include <string>
#include <vector>

#include "AreaController.hpp"
#include "SheetAnnotate.hpp"
#include "opad/json.hpp"

class SheetPage;
class SheetPrintDialog;
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
  // File > Document properties… (and the Drawing tab): the document's own title block fields, one part_properties step.
  void documentProperties();
  // A sheet as PDF, SVG, DXF, DWG or PNG, a drawing ("drawing:<name>") as the PDF pages of its sheets (UI-86): the file's
  // type is the format, the views projected on a worker (ExportJob.hpp); OPAD_BENCH_EXPORT_OUT skips the file dialog.
  // issue: a revision of it as it was issued (its frozen linework, drawing::issued_display).
  void exportSheet(const std::string& id, const std::string& issue = {});
  // Its file types, (format, filter): several sheets only PDF, DWG only while a converter is found (opad::dwg_converter).
  static std::vector<std::pair<QString, QString>> sheetExportTypes(bool several);
  void rowMenu(const std::string& id, QMenu& menu);  // the Drawings folder's menu of a row
  opad::json lastExport;  // the last sheet export's result, or {"error"} (benches)
  static DocsArea* of(const std::vector<AreaController*>& areas);  // the window's one (benches)

  // The Drawings workspace (DocsWorkspace.cpp).
  void newDrawing(std::vector<std::string> nodes = {});  // the dialog; the views measured on a worker; one `sheet` step
  void createDrawing(const opad::json& args, std::function<void(const std::string& sheet)> done = {});  // without the dialog
  void newSheet();         // one more sheet in the shown sheet's drawing, on its paper and template
  void sheetProperties();  // the dialog, then one sheet_edit
  void templateFromFile(const QString& file = {});  // read on a worker; the sheet takes its frame, title block and paper
  void templateFields();   // the shown sheet's template fields placed with the mouse (TemplateFields.hpp), one sheet_edit
  void placeView(const std::string& orient);  // a base view placed with the mouse (front, top, ..., iso)
  void placeProjected();                      // a view projected from the selected one, placed with the mouse
  void setViewStyle(const std::vector<std::string>& views, const opad::json& style);  // merged into each view's style
  void openSheet(const std::string& rowId);   // a Drawings folder row: its sheet in the Drawings workspace
  void viewMenu(const std::vector<std::string>& views, QMenu& menu);  // right-click on views
  // Annotations (DocsAnnotate.cpp).
  void startTool(SheetAnnotator::Tool tool);       // on the shown sheet (New drawing… without one)
  void reattachSelected();                          // the selected annotation's references picked again
  void dimensionFromDatums(const std::string& type);  // ordinate | baseline | chain sets from the view's datums, planned on a worker
  void itemMenu(const std::vector<std::string>& items, QMenu& menu);  // right-click on annotations
  // A balloon on every parts-list row the selected view (else the sheet's pictorial view, else its first) shows, planned on
  // a worker; one step (a parts list comes with it when the drawing has none).
  void autoBalloon();
  void renumberList(const std::string& list);  // 1, 2, ... again in the BoM's order, worked out on a worker; one sheet_edit
  // Issue revision (IssueRevision.cpp): the dialog; then the plan, the PDF (as the drawing will show the revision) and its
  // hash on a worker and one sheet_issue step; tagged: the document saved, committed and tagged in git in the background.
  // done gets sheet_issue's result with "git" ("tagged <tag>" or why not), or {"error"} / null (benches).
  void issueRevision();
  void issue(const opad::json& args, const QString& pdf, bool tagged, std::function<void(const opad::json&)> done = {});
  opad::json lastIssue;
  // Print… (SheetPrint.cpp, UI-86): the shown drawing's sheets drawn on a worker, then the print dialog with its preview;
  // printing runs on a worker. opened gets the dialog (benches); lastPrint: {"pages"} or {"error"} once printed.
  void printSheets(std::function<void(SheetPrintDialog*)> opened = {});
  opad::json lastPrint;
  // A command once nothing reads the document (the sheet's own worker is stopped for it); `then` gets the result, or null
  // when it was refused (a message box said why).
  void run(const std::string& command, opad::json args, std::function<void(const opad::json&)> then = {});
  void whenFree(std::function<void()> fn);
  SheetPage* sheetPage() const { return m_page; }

 private:
  void buildDrawingCommands();
  void drawingsRibbon(RibbonLayout& layout);
  void readyDrawings();
  void buildAnnotateCommands();
  void annotateRibbon(RibbonLayout& layout);
  void readyAnnotate();
  void syncStyleActions();  // Hidden lines / Tangent edges checked as the selected views show them
  void commitAndTag(const QString& rev, const QString& tag, const QString& pdf, std::function<void(const opad::json&)> done);
  SheetPage* m_page = nullptr;
};
