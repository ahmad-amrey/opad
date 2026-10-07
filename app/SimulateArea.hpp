#pragma once
// The Simulate workspace (Ctrl+5): joints between parts and the relations between joints (gears, racks, lead screws),
// the mechanism moved by a joint's slider, loads and supports for structural studies, and studies run and shown: motion
// and dynamic ones played back frame by frame with their series plotted, static and modal ones as a colour map of the
// result over the deformed parts. Everything it writes goes through the core commands (joint, joint_set, load, study).
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <AIS_InteractiveObject.hxx>

#include "AreaController.hpp"
#include "BrowserDelegate.hpp"
#include "opad/sim/kinematics.hpp"
#include "opad/sim/study.hpp"

class Job;
class SimLegend;
class SimulatePanel;
class ToolPanel;

class Simulate : public AreaController {
  Q_OBJECT
 public:
  explicit Simulate(AreaServices& services);
  ~Simulate() override;
  void buildActions() override;
  void ribbon(RibbonLayout& layout) override;
  void ready() override;
  void selectionChanged(const SelectionContext& selection) override;
  void documentChanged(bool replaced) override;
  void workspaceChanged(const QString& id) override;
  void positionOverlays(const QRect& viewport) override;

  // For benches: what is shown.
  SimulatePanel* form() const { return m_form; }
  ToolPanel* panel() const { return m_panel; }
  std::shared_ptr<const opad::sim::StudyRun> shownRun() const { return m_run; }
  const std::string& study() const { return m_study; }
  const std::string& joint() const { return m_joint; }
  int frame() const { return m_frame; }
  bool playing() const { return m_tick.isActive(); }
  bool running() const { return bool(m_job); }
  bool resultShown() const { return !m_map.IsNull(); }
  void open();
  void runStudy();
  void showFrame(int frame);
  void openGuide(int useCase);  // the Simulation guide, at that use case
  void printSettings();  // the Printed part dialog for the structural study shown (or the last one)
  void openCooling(int step = -1);  // the Cooling assistant

 private:
  opad::json write(const std::string& command, const opad::json& args, const QString& label);  // doc->run once it is free
  void refresh();  // the mechanism, its joints and the studies into the panel
  void chooseJoint(const std::string& id);
  void previewJoint(double value);
  void setJoint(double value);
  void addJoint(const QString& kind);
  void newStudy(const QString& kind);
  void chooseStudy(const std::string& id);
  void showRun();
  void showSeries(int index);
  void play();
  void pause();
  void tick();
  void addLoad(const QString& kind);
  opad::Vec3 askVector(const QString& title, const QString& label, bool* ok);  // "x, y, z" from a dialog
  [[noreturn]] void wantPicks(const char* filter, const char* hint);
  void showResults(bool on);
  void clearMotion();
  std::vector<browser::Item> folderItems() const;
  void folderMenu(const std::string& id, QMenu& menu);
  std::vector<std::string> pickedJoints() const;  // joints chosen in the browser's Simulation folder, in order

  SimulatePanel* m_form = nullptr;
  ToolPanel* m_panel = nullptr;
  SimLegend* m_legend = nullptr;
  std::unique_ptr<opad::sim::Mechanism> m_mech;  // the document's mechanism, for the slider's preview
  std::string m_joint;                            // the joint the slider moves
  std::string m_study;                            // the study shown
  std::shared_ptr<const opad::sim::StudyRun> m_run;
  std::string m_runState;  // the document state the run was for
  int m_frame = 0;
  std::vector<std::string> m_mapHidden;  // result bodies the user has hidden: left out of the map (an enclosure, to see inside)
  std::vector<std::string> hiddenResultBodies() const;
  int m_seriesIndex = 0;
  QString m_field = "von_mises";
  QPointer<Job> m_job;
  QTimer m_tick;
  QElapsedTimer m_clock;
  double m_playFrom = 0;  // the run's time when the play started
  Handle(AIS_InteractiveObject) m_map;
  std::vector<std::string> m_picked;  // browser rows picked (ids as the folder gives them)
};
