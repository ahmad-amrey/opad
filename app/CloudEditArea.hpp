#pragma once
// Edit point cloud (CloudEditArea.cpp): a mode, entered and left like sketch mode, in which a scan used as the reference of
// a reverse-engineered part is sculpted by hand: Push/Pull and Smooth brushes, a Delete brush, Add points on the local
// surface, and Select (lasso, box or brush) with Delete these / Keep only these. Its own tab comes first in Design while it
// runs (design.cloudEdit), Finish writes one cloud_edit step (opad/cloud_edit.hpp) and Cancel drops the changes (asked
// first when there are some); Esc never leaves. Undo and Redo go stroke by stroke inside it (AreaController::history).
//
// The work is the Editor's (core), on workers, one task at a time in order (a brush's dabs coalesce while one runs): the
// UI thread only picks (navigationPoint through the cloud's CloudPickSet) and draws what the workers made, the stroke under
// way over the cloud (Viewport::showCloudStroke) and, at the end of each stroke, the cloud as the working edit leaves it
// (Viewport::setCloudEdit, kept through the level-of-detail passes). Editing an existing step rolls the timeline back to it.
#include <Graphic3d_ArrayOfPoints.hxx>
#include <QPointF>
#include <QPolygonF>
#include <deque>
#include <memory>

#include "AreaController.hpp"
#include "opad/cloud_edit.hpp"

class Job;
class PanelFooter;
class QAbstractButton;
class QLabel;
class QSlider;
class QWidget;

class CloudEditArea : public AreaController {
 public:
  enum class Tool { Pull, Smooth, Erase, Add, Select };
  using AreaController::AreaController;
  ~CloudEditArea() override;

  void buildActions() override;
  void menus(QMenuBar* bar, const QMap<QString, QMenu*>& menus) override;
  void ribbon(RibbonLayout& layout) override;
  void ready() override;
  void contextMenu(const SelectionContext& selection, QMenu& menu) override;
  void documentChanged(bool replaced) override;
  bool maybeClose() override;
  bool history(bool& canUndo, bool& canRedo) const override;
  bool command(const QString& id, const SelectionContext& selection) override;

  // Benches (CloudEditBench.cpp) and the window.
  bool active() const { return m_active; }
  bool prepared() const { return m_editor != nullptr; }
  bool working() const { return m_job != nullptr || !m_tasks.empty() || m_dragging; }
  const std::string& cloud() const { return m_body; }
  size_t step() const { return m_index; }                  // strokes kept (the history's place)
  size_t steps() const { return m_history.size(); }
  opad::json summary() const;                              // the working edit's counts
  std::shared_ptr<const opad::cloud::Edit> current() const { return m_history.empty() ? nullptr : m_history[m_index]; }
  size_t selected() const { return m_selected.count(); }
  void setTool(Tool tool);
  Tool tool() const { return m_tool; }
  int radiusPixels() const;
  void setRadiusPixels(int pixels);
  void edit(const std::string& body, const std::string& opId = {});  // enters the mode (an op: that step edited again)
  void finish();
  void cancel(bool ask = true);

 protected:
  bool eventFilter(QObject* object, QEvent* event) override;

 private:
  struct Dab {
    Tool tool = Tool::Pull;
    opad::Vec3 centre{0, 0, 0}, toward{0, 0, 1};
    double radius = 1, amount = 0;
  };
  struct Out {
    std::shared_ptr<const opad::cloud::Edit> snapshot;
    std::vector<uint32_t> ids;
    opad::cloud::Region region;
    Handle(Graphic3d_ArrayOfPoints) stroke;
    size_t count = 0;
  };
  struct Task {
    std::vector<Dab> dabs;  // a brush's (more join while it waits)
    std::shared_ptr<std::vector<uint32_t>> stroke;  // the stroke the dabs belong to: the points it touched so far
    std::function<void(opad::cloud::Editor&, Out&)> work;
    std::function<void(Out&)> done;
  };
  std::string chosenCloud(const SelectionContext& selection) const;
  bool cloudBody(const std::string& id) const;
  void enterPrepared(const std::string& body, const std::string& opId, std::shared_ptr<const opad::cloud::Edit> start);
  void leave();
  void buildPanel();
  void refresh();
  void queue(Task task);
  void pump();
  void queueDab(const QPointF& at);
  void endStroke();
  void finishSelect();
  void changeSelection(bool keep);
  void undo();
  void redo();
  void showBrush(const QPointF& at);
  bool frameAt(const QPointF& at, opad::Vec3& local, opad::Vec3& toward, double& mmPerPixel);
  double strength() const;
  QAction* toolAction(Tool tool) const;

  QAction *m_edit = nullptr, *m_finish = nullptr, *m_cancel = nullptr;
  std::vector<QAction*> m_toolActions;
  class ToolPanel* m_panel = nullptr;
  PanelFooter* m_footer = nullptr;
  std::vector<QAbstractButton*> m_selectModes;
  QAbstractButton *m_pull = nullptr, *m_push = nullptr, *m_deleteSel = nullptr, *m_keepSel = nullptr;
  QWidget *m_directionRow = nullptr, *m_strengthRow = nullptr, *m_selectRow = nullptr;
  QSlider *m_radius = nullptr, *m_strength = nullptr;
  QLabel *m_radiusLabel = nullptr, *m_status = nullptr, *m_hint = nullptr;

  bool m_active = false, m_dragging = false, m_finishing = false, m_inverted = false;
  int m_session = 0;      // a mode entered anew: what the workers of the last one hand back is dropped
  int m_selectMode = 0;   // 0 lasso, 1 box, 2 brush
  int m_combine = 0;      // a selection drag: 1 adds (Shift), -1 takes away (Ctrl), 0 replaces
  Tool m_tool = Tool::Pull;
  std::string m_body, m_op;
  QString m_workspaceBefore;
  opad::Mat4 m_world;     // the cloud's placement
  std::shared_ptr<const class BodyPrs> m_base;
  std::shared_ptr<opad::cloud::Editor> m_editor;
  std::vector<std::shared_ptr<const opad::cloud::Edit>> m_history;  // [0] as the mode began, then one a stroke
  size_t m_index = 0;
  std::deque<Task> m_tasks;
  Job* m_job = nullptr;
  bool m_taskRunning = false;
  // The stroke under way: its points so far (the worker's), the last dab's place, the selection's drag.
  std::shared_ptr<std::vector<uint32_t>> m_strokeIds, m_lastSelectIds;
  bool m_haveLast = false;
  opad::Vec3 m_last{0, 0, 0};
  QPointF m_lastAt;
  QPolygonF m_lasso;
  opad::cloud::Region m_selected;
};
