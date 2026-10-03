#pragma once
// The documentation area (TODO 11, track t5a: drawings, part properties, bills of materials, 2D export) as a feature area
// (AreaController.hpp): File > Export bill of materials and Inspect > Part properties with their menu, ribbon and context
// menu places, the browser's Drawings folder (DrawingsFolder.hpp: rename in place, Del, the rows' menu with Export
// sheet… / Export drawing…), the Properties panel's PART section with its link, and the dialogs and jobs they start.
#include <string>
#include <vector>

#include "AreaController.hpp"
#include "opad/json.hpp"

class DocsArea : public AreaController {
  Q_OBJECT
 public:
  using AreaController::AreaController;
  void buildActions() override;
  void menus(QMenuBar* bar, const QMap<QString, QMenu*>& menus) override;
  void ribbon(RibbonLayout& layout) override;
  void ready() override;
  void contextMenu(const SelectionContext& selection, QMenu& menu) override;

  // From the PART section's link, the context menu or Inspect > Part properties: the bodies and components of `ids`.
  void editPartProperties(std::vector<std::string> ids);
  void exportBom(std::vector<std::string> ids = {});  // File > Export bill of materials: the dialog, then where to
  // A sheet as PDF, SVG, DXF, DWG or PNG, a drawing ("drawing:<name>") as the PDF pages of its sheets (UI-86): the file's
  // type is the format, the views projected on a worker (ExportJob.hpp); OPAD_BENCH_EXPORT_OUT skips the file dialog.
  void exportSheet(const std::string& id);
  void rowMenu(const std::string& id, QMenu& menu);  // the Drawings folder's menu of a row
  opad::json lastExport;  // the last sheet export's result, or {"error"} (benches)
  static DocsArea* of(const std::vector<AreaController*>& areas);  // the window's one (benches)
};
