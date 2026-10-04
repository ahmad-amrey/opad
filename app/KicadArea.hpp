#pragma once
// KiCad boards in the window (UI-72 UI, UI-134): Insert KiCad PCB… links a board (its options asked first, monitored with
// its 3D models by AssetMonitor); the sync preview reads a changed board on a worker and lists what syncing would do to it,
// per reference designator (moved, turned, flipped, model or footprint changed, added, removed, the mounting holes and the
// board itself; any other linked file per body, opad::asset_sync_parts) and the design built on it (opad::asset_sync_affects
// of the sync's plan: sketches with the references that move or are projected again and their dimensions, features
// recomputed, new errors), the parts it names tinted in the view, Sync in its footer committing that plan; Project KiCad
// board puts the board's outline, its mounting holes and chosen parts into the open sketch as references by node, which every
// sync keeps (opad design::derive_sketch, "asset" sources); Check clearance to board lists the board's parts too close to its
// enclosure; a board's Properties say it explodes as one; Hide small parts while navigating (the viewport's small-part
// filter, its size a setting) keeps orbiting a dense board fluid.
#include <QPointer>
#include <QString>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "AreaController.hpp"
#include "PropertiesPanel.hpp"
#include "opad/util.hpp"

class AssetsArea;
class CheckPanel;
class Job;
class PanelFooter;
class QDialog;
class QTreeWidget;
class ToolPanel;
namespace opad::design {
struct Plan;
}

class KicadArea : public AreaController {
  Q_OBJECT
 public:
  explicit KicadArea(AreaServices& services);
  void buildActions() override;
  void menus(QMenuBar* bar, const QMap<QString, QMenu*>& menus) override;
  void ribbon(RibbonLayout& layout) override;
  void ready() override;
  void contextMenu(const SelectionContext& selection, QMenu& menu) override;
  void documentChanged(bool replaced) override;

  // A KiCad board of the document as the scene has it: its top node, its Outline layer and Mounting holes component (empty
  // when the import has none: an older one, or KiCad's own export read before it had them), the holes and the parts (footprints'
  // components).
  struct Board {
    std::string import, root, outline, holes;
    QString name;
    bool linked = false;
    std::vector<std::pair<std::string, QString>> holeNodes, parts;  // node id, name
  };
  std::vector<std::string> boards() const { return m_boards; }  // the document's KiCad imports, in log order
  Board board(const std::string& import) const;
  std::string boardOf(const SelectionContext& selection) const;  // the KiCad import the selection is part of

  void insert(const QString& file = {});  // Insert KiCad PCB…: a file dialog unless given, the board's options, linked
  // Reads the linked board and plans its sync on a worker (a copy of the document; `waited`: tries while it is busy), then the
  // panel: the board's changes and the design they affect.
  void preview(const std::string& import, int waited = 0);
  void sync();  // Sync from the preview's footer: the plan previewed while the document is as it was, else planned again
  void closePreview();
  QDialog* project();  // Project KiCad board…: the dialog (window-modal, not blocking); null when no sketch is open
  // Board outline, mounting holes (all, as one reference: holes added later come with a sync) and parts (node ids) of
  // `import` into the open sketch; false when the sketch is busy.
  bool projectInto(const std::string& import, bool outline, bool holes, const std::vector<std::string>& parts, bool linked);
  // Check clearance to board…: the board's solids against the selection outside it (else every other visible solid), pairs
  // overlapping or closer than the gap (setting kicad/clearance, 1 mm) listed in the "kicadClearance" panel, a row selecting
  // the pair and measuring its gap; run again from the panel's Check.
  void clearance(const std::string& import);
  void runClearance(int waited = 0);
  void setSmallParts(bool on);    // the filter on or off (setting view/hideSmallParts)
  void askSmallPartSize();        // its size (setting view/smallPartSize, mm)

  ToolPanel* previewPanel() const { return m_panel; }
  QTreeWidget* previewList() const { return m_list; }
  PanelFooter* previewFooter() const { return m_footer; }
  const opad::json& previewReport() const { return m_report; }
  const std::string& previewImport() const { return m_import; }
  bool previewPlanned() const { return m_plan != nullptr; }  // Sync commits what was previewed
  ToolPanel* clearancePanel() const { return m_clearancePanel; }
  CheckPanel* clearanceChecks() const { return m_checks; }
  const opad::json& lastClearance() const { return m_lastClearance; }  // check_interference's result

 signals:
  void previewReady(bool ok, const QString& error);
  void clearanceReady(bool ok);

 private:
  void buildPanel();
  void fill();
  void fillAffected();
  void buildClearancePanel();
  void section(const PropertySubject& subject, QList<PropertySection>& out);
  AssetsArea* assets() const;
  std::vector<std::string> m_boards;
  ToolPanel* m_panel = nullptr;
  QTreeWidget* m_list = nullptr;
  PanelFooter* m_footer = nullptr;
  std::string m_import;  // the board previewed
  opad::json m_report;
  std::shared_ptr<opad::design::Plan> m_plan;  // its sync, planned on the document at m_planRevision
  unsigned long long m_planRevision = 0;
  QPointer<Job> m_job;
  bool m_reading = false;  // a preview is copying the document, reading or waiting to
  ToolPanel* m_clearancePanel = nullptr;
  CheckPanel* m_checks = nullptr;
  std::string m_clearanceImport;
  std::vector<std::string> m_enclosure;  // what the board is checked against; empty: every other visible solid
  opad::json m_lastClearance;
  QPointer<Job> m_checkJob;
  bool m_measured = false;  // a finding's gap is shown in the view
};
