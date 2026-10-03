#pragma once
// The timeline's own commands (TODO 11 UI-99), a feature area (AreaController.hpp): names on the markers (setting
// timeline/names), the design history alone (timeline/designOnly), rolling the model back with the playhead or Roll back to
// here (the timeline's marker menu) and forward again. While it is rolled back a chip over the view says so and rolls
// forward on a click; any change made meanwhile is appended at the end and rolls forward first (AppDocument::rollBackTo),
// and a command that edits the document drops the picks first (they are faces of the bodies as they were). The marker
// hover and click linkage to the geometry is smart selection's (SmartSelect).
#include <string>

#include "AreaController.hpp"

class QToolButton;

class TimelineArea : public AreaController {
  Q_OBJECT
 public:
  using AreaController::AreaController;
  void buildActions() override;
  void menus(QMenuBar* bar, const QMap<QString, QMenu*>& menus) override;
  void ready() override;
  void documentChanged(bool replaced) override;
  // Before `op` (empty: the end), unless an editor is open or the design is busy (then said in the status bar): false.
  bool rollTo(const std::string& op);
  QToolButton* chip() const { return m_chip; }

 private:
  void refresh();
  QAction *m_names = nullptr, *m_designOnly = nullptr, *m_forward = nullptr;
  QToolButton* m_chip = nullptr;
};
