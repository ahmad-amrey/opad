#pragma once
// Image canvases in the window (UI-70, opad/canvas.hpp): Insert canvas puts a picture on the selected planar face or a picked
// plane, placed by DrawingPlacer with an offset and a width; the canvas panel gives its centre (X, Y), width, height (one
// typed keeps its proportions, both stretch it) and angle (Tab goes round them, Enter applies; digits typed in the view
// while it is open land in them), Picture proportions while it is stretched, its opacity, flips,
// show-through, selectable and lock, and runs Calibrate (two points on the picture, then their real distance) and Align to
// model (two points on the picture onto two vertices or circle centres of the model) as guided tools (a prompt bar, their
// steps in the panel, Back and Esc stepping back), Trace to sketch and Replace picture; the
// on-canvas handles are CanvasEditor's, a double click on a canvas opens it, its context menu has the same; a sketch's
// backdrop images become canvases. Moving, sizing, turning, calibrating, aligning and the flags are one op each through the
// canvas command; replace, backdrops and trace are planned on a worker; each is one undo step.
#include <QPointer>
#include <array>
#include <functional>
#include <string>
#include <vector>

#include "AreaController.hpp"
#include "PropertiesPanel.hpp"
#include "opad/canvas.hpp"

class CanvasEditor;
class DrawingPlacer;
class ToolStepsPanel;
class PanelFooter;
class PromptBar;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class ToolPanel;

class CanvasArea : public AreaController {
  Q_OBJECT
 public:
  enum class Flow { None, Calibrate, Align };
  explicit CanvasArea(AreaServices& services);
  void buildActions() override;
  void menus(QMenuBar* bar, const QMap<QString, QMenu*>& menus) override;
  void ribbon(RibbonLayout& layout) override;
  void ready() override;
  void contextMenu(const SelectionContext& selection, QMenu& menu) override;
  void selectionChanged(const SelectionContext& selection) override;
  void positionOverlays(const QRect& viewport) override;
  void documentChanged(bool replaced) override;

  void insert(const QString& file = {});  // a file dialog when none is given; then the plane, then the placer
  void placeOn(const QString& file, const opad::Frame& plane, double u, double v, bool link);  // the placer on this plane, centred at (u, v)
  void edit(const std::string& canvas);  // its handles and the panel
  void finish();
  void calibrate();
  void align();
  void trace();
  void replace(const QString& file = {});
  void fromBackdrop(const std::string& sketch);
  std::string canvasOf(const SelectionContext& selection) const;  // the one canvas selected (and nothing else), else empty
  std::string target() const;  // the canvas being edited, else the one selected
  bool run(opad::json args);   // the canvas command on the target: one op, one undo step; false (and a toast) when refused
  CanvasEditor* editor() const { return m_editor; }
  DrawingPlacer* placer() const { return m_placer; }
  ToolPanel* panel() const { return m_panel; }
  QLineEdit* field(int i) const;  // 0-4: X, Y, width, height, angle; 5: the real distance of Calibrate
  ToolStepsPanel* stepsPanel() const { return m_steps; }  // Calibrate's and Align's steps (shown while one runs)
  PanelFooter* footer() const { return m_footer; }
  Flow flow() const { return m_flow; }
  const std::vector<opad::Vec3>& flowPoints() const { return m_points; }

 signals:
  void flowEnded(bool done);  // Calibrate or Align applied (true) or left
  void traced(bool ok, const QString& error);
  void planDone(bool ok, const QString& error);  // a replace or a backdrop conversion committed, or not

 protected:
  bool eventFilter(QObject* object, QEvent* event) override;

 private:
  void buildPanel();
  void section(const PropertySubject& subject, QList<PropertySection>& out);  // Properties of a canvas
  void fillPanel(bool force = false);
  void applyFields();
  void setFlags(const opad::json& change);
  void startFlow(Flow flow);
  void endFlow(bool done = false);
  void flowPicked(const opad::Vec3& point, bool model);
  void flowBack();
  void nextPick();
  void refreshPrompt();
  void applyCalibration();  // the real distance typed: one transform op, the flow ends
  void footerForFlow();     // the panel's page and footer for the flow (Back, Cancel, Apply) or the canvas (Close)
  void afterInsert();  // a linked picture loaded: its canvas edited
  void commitInsert(std::shared_ptr<opad::design::Plan> plan, int waited = 0);  // a copy read on a worker: committed, then edited
  void toast(const QString& text, int ms = 5000);
  // Plans on a worker against a copy of the document and commits the plan as one undo step (`label`).
  void planned(const QString& title, const QString& label, std::function<opad::design::Plan(opad::Document&)> plan,
               std::function<void(bool, const QString&, const opad::json&)> then);
  bool sketchWithBackdrops(const SelectionContext& selection, std::string& sketch) const;

  CanvasEditor* m_editor = nullptr;
  DrawingPlacer* m_placer = nullptr;
  ToolPanel* m_panel = nullptr;
  QPointer<PromptBar> m_prompt;
  QLabel* m_hint = nullptr;
  QWidget *m_main = nullptr, *m_flowPage = nullptr;
  ToolStepsPanel* m_steps = nullptr;
  std::array<QLineEdit*, 5> m_fields{};
  QLineEdit* m_distance = nullptr;
  QWidget* m_distanceRow = nullptr;
  QSlider* m_opacity = nullptr;
  QCheckBox *m_flipH = nullptr, *m_flipV = nullptr, *m_through = nullptr, *m_selectable = nullptr, *m_lock = nullptr;
  QList<QPushButton*> m_moves;  // Calibrate and Align: off while locked
  QPushButton* m_proportions = nullptr;  // shown while it is stretched
  PanelFooter* m_footer = nullptr;
  Flow m_flow = Flow::None;
  std::vector<opad::Vec3> m_points;
  int m_typed = 0;      // a key claimed at the shortcut stage: its press is swallowed
  QPointer<QLineEdit> m_typing;  // the field typed into from the view (the first key replaces its text, the next ones add)
  bool m_filling = false;
  bool m_busy = false;  // a plan is being made
};
