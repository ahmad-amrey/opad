#pragma once
// The timeline's own commands (TODO 11 UI-99), a feature area (AreaController.hpp): names on the markers (setting
// timeline/names), the design history alone (timeline/designOnly), rolling the model back with the playhead or Roll back to
// here (the timeline's marker menu) and forward again. While it is rolled back a chip over the view says so and rolls
// forward on a click; any change made meanwhile is appended at the end and rolls forward first (AppDocument::rollBackTo),
// and a command that edits the document drops the picks first (they are faces of the bodies as they were). The marker
// hover and click linkage to the geometry is smart selection's (SmartSelect).
// The history as a list (timeline.historyList, setting timeline/historyList): the browser's History folder has a row per
// marker, top to bottom, and one for the roll-back marker while rolled back; the steps after it are greyed, tombstoned
// ones in italics, failing ones badged. A row points at its step (the marker current and pulsing, what it made in amber,
// as under the pointer), a double-click edits a feature or a sketch (on the roll-back row: rolls forward), its menu is
// the marker's, Del deletes it as Del on the marker does.
#include <map>
#include <set>
#include <string>
#include <vector>

#include "AreaController.hpp"
#include "BrowserDelegate.hpp"

class QToolButton;

class TimelineArea : public AreaController {
  Q_OBJECT
 public:
  using AreaController::AreaController;
  void buildActions() override;
  void menus(QMenuBar* bar, const QMap<QString, QMenu*>& menus) override;
  void ready() override;
  void documentChanged(bool replaced) override;
  void selectionChanged(const SelectionContext& selection) override;
  bool command(const QString& id, const SelectionContext& selection) override;
  // Before `op` (empty: the end), unless an editor is open or the design is busy (then said in the status bar): false.
  bool rollTo(const std::string& op);
  QToolButton* chip() const { return m_chip; }
  // The History folder's rows (benches): ids "history:<op id>", and kRollRow for the roll-back marker.
  static constexpr const char* kRollRow = "history:@rollback";
  static std::string rowOp(const std::string& id);  // the op of a step's row, empty for any other id
  std::vector<browser::Item> historyRows();
  void rowMenu(const std::string& id, QMenu& menu);
  void rowActivated(const std::string& id);

 private:
  void refresh();
  void decorate(const browser::Row& row, browser::Decoration& out) const;
  QAction *m_names = nullptr, *m_designOnly = nullptr, *m_forward = nullptr, *m_list = nullptr;
  QToolButton* m_chip = nullptr;
  std::set<std::string> m_beyond, m_tombstoned, m_suppressed;  // rows' states as of the last historyRows()
  std::map<std::string, QString> m_failing;                     // row -> its feature's error
  std::string m_pointed;                         // the step a selected row shows in amber
};
