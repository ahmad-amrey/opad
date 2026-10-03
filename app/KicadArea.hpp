#pragma once
// KiCad boards in the window (UI-72 UI): Insert KiCad PCB… links a board (its options asked first, monitored with
// its 3D models by AssetMonitor); the sync preview reads a changed board on a worker and lists what syncing would do to it,
// per reference designator (moved, turned, flipped, model or footprint changed, added, removed, the mounting holes and the
// board itself), the parts it names tinted in the view, Sync in its footer.
#include <QPointer>
#include <QString>
#include <string>
#include <utility>
#include <vector>

#include "AreaController.hpp"
#include "opad/util.hpp"

class AssetsArea;
class Job;
class PanelFooter;
class QTreeWidget;
class ToolPanel;

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
  // when the import has none: an older one, or KiCad's own export), the holes and the parts (footprints' components).
  struct Board {
    std::string import, root, outline, holes;
    QString name;
    bool linked = false;
    std::vector<std::pair<std::string, QString>> holeNodes, parts;  // node id, name
  };
  std::vector<std::string> boards() const { return m_boards; }  // the document's KiCad imports, in log order
  Board board(const std::string& import) const;
  std::string boardOf(const SelectionContext& selection) const;  // the KiCad import the selection is part of

  void insert(const QString& file = {});    // Insert KiCad PCB…: a file dialog unless given, the board's options, linked
  void preview(const std::string& import);  // reads the linked board on a worker, then the panel
  void sync();                              // Sync from the preview's footer
  void closePreview();

  ToolPanel* previewPanel() const { return m_panel; }
  QTreeWidget* previewList() const { return m_list; }
  PanelFooter* previewFooter() const { return m_footer; }
  const opad::json& previewReport() const { return m_report; }
  const std::string& previewImport() const { return m_import; }

 signals:
  void previewReady(bool ok, const QString& error);

 private:
  void buildPanel();
  void fill();
  AssetsArea* assets() const;
  std::vector<std::string> m_boards;
  ToolPanel* m_panel = nullptr;
  QTreeWidget* m_list = nullptr;
  PanelFooter* m_footer = nullptr;
  std::string m_import;  // the board previewed
  opad::json m_report;
  QPointer<Job> m_job;
};
