#pragma once
// Edit point cloud (CloudEditArea.cpp): a mode, entered and left like sketch mode, in which a scan used as the reference of
// a reverse-engineered part is sculpted by hand: Push/Pull and Smooth brushes (three falloffs), a Delete brush, Add points
// on the local surface, and selecting (Paint, Box, Lasso, Grow region over the scan's own surface) then Delete selected /
// Keep only selected. Its own tab comes first in Design while it runs (design.cloudEdit), Finish writes one cloud_edit step
// (opad/cloud_edit.hpp) and Cancel drops the changes (asked first when there are some); Esc never leaves. Undo and Redo go
// stroke by stroke inside it (AreaController::history).
//
// The work is the Editor's (core), on workers, one task at a time in order (a brush's dabs coalesce while one runs): the
// UI thread only picks (navigationPoint through the cloud's CloudPickSet) and draws what the workers made, the stroke under
// way over the cloud (Viewport::showCloudStroke) and, at the end of each stroke, the cloud as the working edit leaves it
// (Viewport::setCloudEdit, kept through the level-of-detail passes). Editing an existing step rolls the timeline back to it.
#include <Graphic3d_ArrayOfPoints.hxx>
#include <QPointF>
#include <QPolygonF>
#include <QString>
#include <deque>
#include <functional>
#include <memory>

#include "AreaController.hpp"
#include "opad/cloud_edit.hpp"

class BodyPrs;
class Job;
class PanelFooter;
class PromptBar;
class QAbstractButton;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QSlider;
class QWidget;
class ToolPanel;

class CloudEditArea : public AreaController {
 public:
  enum class Tool { Pull, Smooth, Erase, Add, Paint, Box, Lasso, Grow };
  using AreaController::AreaController;
  ~CloudEditArea() override;

  void buildActions() override;
  void menus(QMenuBar* bar, const QMap<QString, QMenu*>& menus) override;
  void ribbon(RibbonLayout& layout) override;
  void ready() override;
  void contextMenu(const SelectionContext& selection, QMenu& menu) override;
  void positionOverlays(const QRect& viewport) override;
  void documentChanged(bool replaced) override;
  bool maybeClose() override;
  bool history(bool& canUndo, bool& canRedo) const override;
  bool command(const QString& id, const SelectionContext& selection) override;

  // Benches (CloudEditBench.cpp) and the window.
  bool active() const { return m_active; }
  bool prepared() const { return m_editor != nullptr; }
  bool working() const { return m_taskRunning || !m_tasks.empty() || m_dragging || m_finishing; }
  const std::string& cloud() const { return m_body; }
  size_t step() const { return m_index; }  // strokes kept (the history's place)
  size_t steps() const { return m_history.size(); }
  opad::json summary() const;              // the working edit's counts
  std::shared_ptr<const opad::cloud::Edit> current() const { return m_history.empty() ? nullptr : m_history[m_index]; }
  size_t selected() const { return m_selected.count(); }
  void setTool(Tool tool);
  Tool tool() const { return m_tool; }
  int radiusPixels() const;
  void setRadiusPixels(int pixels);
  void edit(const std::string& body, const std::string& opId = {});  // enters the mode (an op: that step edited again)
  void finish();
  void cancel(bool ask = true);
  void changeSelection(bool keep);  // Delete selected / Keep only selected

 protected:
  bool eventFilter(QObject* object, QEvent* event) override;

 private:
  struct Dab {
    Tool tool = Tool::Pull;
    opad::cloud::Brush brush;
    double amount = 0;
  };
  struct Out {
    std::shared_ptr<const opad::cloud::Edit> snapshot;
    opad::cloud::Region region;
    Handle(Graphic3d_ArrayOfPoints) stroke;
    size_t count = 0;
    bool stopped = false;
    std::function<bool()> cancelled;  // a shown task's Cancel (the status bar's)
  };
  struct Task {
    std::vector<Dab> dabs;                          // a brush's (more join while it waits)
    std::shared_ptr<std::vector<uint32_t>> stroke;  // the stroke the dabs belong to: the points it touched so far
    std::function<void(opad::cloud::Editor&, Out&)> work;
    std::function<void(Out&)> done;
    QString shown;  // a title: the task shows its progress and can be cancelled (Grow region); empty: quiet
  };
  static bool selecting(Tool t) { return t >= Tool::Paint; }
  static bool brushes(Tool t) { return t <= Tool::Paint; }
  std::string chosenCloud(const SelectionContext& selection) const;
  bool cloudBody(const std::string& id) const;
  void enterPrepared(std::shared_ptr<const opad::cloud::Edit> begun);
  void leave();
  void buildPanel();
  void refresh();
  void queue(Task task);
  void pump();
  void queueDab(const QPointF& at);
  void endStroke();
  void finishSelect(const QPointF& at);
  void kept(const std::shared_ptr<const opad::cloud::Edit>& snapshot);  // a stroke done: into the history, drawn
  void undo();
  void redo();
  void showBrush(const QPointF& at);
  bool frameAt(const QPointF& at, opad::Vec3& local, opad::Vec3& toward, double& mmPerPixel);
  double strength() const;
  opad::cloud::Falloff falloff() const;
  bool frontOnly() const;
  QAction* toolAction(Tool tool) const { return m_toolActions[size_t(tool)]; }

  QAction *m_edit = nullptr, *m_finish = nullptr, *m_cancel = nullptr, *m_deleteSelected = nullptr, *m_keepSelected = nullptr;
  std::vector<QAction*> m_toolActions;  // by Tool
  ToolPanel* m_panel = nullptr;
  PanelFooter* m_footer = nullptr;
  PromptBar* m_prompt = nullptr;
  std::vector<QAbstractButton*> m_toolButtons, m_selectButtons, m_falloffButtons;
  QAbstractButton *m_pull = nullptr, *m_push = nullptr, *m_deleteSel = nullptr, *m_keepSel = nullptr;
  QWidget *m_directionRow = nullptr, *m_sizeRow = nullptr, *m_strengthRow = nullptr, *m_falloffRow = nullptr, *m_selectRow = nullptr,
          *m_growBox = nullptr, *m_selectedRow = nullptr;
  QCheckBox* m_front = nullptr;
  QDoubleSpinBox *m_growAngle = nullptr, *m_growOffset = nullptr;
  QSlider *m_radius = nullptr, *m_strength = nullptr;
  QLabel *m_radiusLabel = nullptr, *m_status = nullptr, *m_hint = nullptr, *m_addSpacing = nullptr, *m_selectedText = nullptr;

  bool m_active = false, m_dragging = false, m_finishing = false, m_inverted = false, m_taskRunning = false;
  int m_session = 0;  // a mode entered anew: what the workers of the last one hand back is dropped
  int m_combine = 0;  // a selection drag: 1 adds (Shift), -1 takes away (Ctrl), 0 replaces
  Tool m_tool = Tool::Pull, m_lastSelect = Tool::Paint;
  std::string m_body, m_op;
  QString m_workspaceBefore;
  opad::Mat4 m_world;  // the cloud's placement
  std::shared_ptr<const BodyPrs> m_base;
  std::shared_ptr<opad::cloud::Editor> m_editor;
  std::vector<std::shared_ptr<const opad::cloud::Edit>> m_history;  // [0] as the mode began, then one a stroke
  size_t m_index = 0;
  std::deque<Task> m_tasks;
  Job* m_job = nullptr;
  // The stroke under way: its points so far (the worker's), the last dab's place, a selection's drag.
  std::shared_ptr<std::vector<uint32_t>> m_strokeIds;
  bool m_haveLast = false;
  opad::Vec3 m_last{0, 0, 0};
  QPolygonF m_lasso;
  opad::cloud::Region m_selected;
};
