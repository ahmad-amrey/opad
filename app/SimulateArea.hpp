#pragma once
// The Simulate workspace (Ctrl+5), for people who are not engineers first: its tabs are goals (Start, Air and fluids, Heat,
// Strength, Movement), each holding checks asked as plain questions (SimChecks) that open a guided panel (GuidedCheck);
// Start is a page of them all in the viewport's place (SimulateStart). Advanced keeps every raw tool below, unchanged:
// joints between parts and the relations between joints (gears, racks, lead screws),
// the mechanism moved by a joint's slider, loads and supports for structural studies, and studies run and shown: motion
// and dynamic ones played back frame by frame with their series plotted, static and modal ones as a colour map of the
// result over the deformed parts. Everything it writes goes through the core commands (joint, joint_set, load, study).
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>

#include <map>
#include <memory>
#include <set>
#include <optional>
#include <string>
#include <vector>

#include <AIS_InteractiveObject.hxx>

#include "SimulatePanel.hpp"
#include "AreaController.hpp"
#include "BrowserDelegate.hpp"
#include "opad/sim/kinematics.hpp"
#include "opad/sim/study.hpp"

class GuidedCheck;
class Job;
class SimLegend;
class SimulateStart;
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
  void tabChanged(const QString& tabId) override;  // Start shows its page in the view's place; a goal tab its check
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
  void openCooling(int step = -1);  // Thermal setup (the CoolingAssistant)
  // The checks (SimChecks): one opened in its guided panel (a new one, or that study of it), the start page shown.
  void openCheck(const QString& check, const std::string& study = {});
  void showStart();
  GuidedCheck* check() const { return m_check; }
  ToolPanel* checkPanel() const { return m_checkPanel; }
  SimulateStart* start() const { return m_start; }
  const opad::json& engines() const { return m_engines; }  // sim::engines() as last read on a worker
  void refreshEngines();  // read again (a solver found by hand)
  void askSolver(const QString& solver);  // the missing-solver window
  // What the Simulation folder's rows picked in the browser light in the view (SimulateLit.cpp): each target with its nature
  // ("heat", "fan", "heatsink", "board", "box", a load's kind), drawn in its colour with its badge when it is shown.
  struct Lit {
    std::string row, nature;
    opad::Ref target;
    bool shown = false;  // in the view (a hidden part is not lit)
  };
  const std::vector<Lit>& lit() const { return m_lit; }
  static QColor natureColor(const std::string& nature);
  static QString natureIcon(const std::string& nature);
  void lightRefs(const std::vector<std::pair<opad::Ref, std::string>>& refs);  // those (a nature each) lit; none: nothing lit
  // Fans as loads have them ({refs, vector, outlet}): each one's plane and the air's way drawn (Viewport::showSimGlyphs).
  void showFanGlyphs(const std::vector<opad::json>& fans);
  size_t fanGlyphsShown() const;

 private:
  opad::json write(const std::string& command, const opad::json& args, const QString& label);  // doc->run once it is free
  void refresh();  // the mechanism, its joints and the studies into the panel
  void chooseJoint(const std::string& id);
  void previewJoint(double value);
  void setJoint(double value);
  void addJoint(const QString& kind);
  void newStudy(const QString& kind);
  void chooseStudy(const std::string& id);
  void showMode(SimulatePanel::Mode mode);  // the panel's sections and studies for a tab
  void followStudy(const std::string& id);  // the tab (and the panel) of that study's kind
  SimulatePanel::Mode studyMode(const opad::Study& study) const;
  void showRun();
  void showSeries(int index);
  void play();
  void pause();
  void tick();
  void addLoad(const QString& kind);
  opad::Vec3 askVector(const QString& title, const QString& label, bool* ok);  // "x, y, z" from a dialog
  [[noreturn]] void wantPicks(const char* filter, const char* hint);
  void showResults(bool on);
  void showFlow(const opad::sim::FeaResult& r);  // a flow study's map: the cut through the fluid by speed or pressure, or streamlines
  void clearMotion();
  std::vector<browser::Item> folderItems() const;
  void folderMenu(const std::string& id, QMenu& menu);
  std::vector<std::string> pickedJoints() const;  // joints chosen in the browser's Simulation folder, in order
  std::vector<browser::Item> setupRows(const opad::Study& study) const;  // a thermal study's box, boards, heatsinks and air
  // Del (and Delete in the menu) on the folder's rows: loads, a whole case, studies, joints, a study's box, board or heatsink
  // (its settings changed); one step to undo. False: nothing there could be deleted (the Air row).
  bool removeRows(const std::vector<std::string>& rows);
  std::vector<Lit> rowTargets(const std::string& row) const;
  void lightRows(const std::vector<std::string>& rows);  // the rows' targets lit (none: what was lit goes)
  void lightTargets(std::vector<Lit> targets);
  void addTarget(std::vector<Lit>& out, const std::string& row, const opad::Ref& r, const std::string& nature) const;  // a component: its bodies
  void placeBadges();  // over their targets, after every camera move
  std::vector<Lit> m_lit;
  std::vector<std::pair<opad::Ref, std::string>> m_litRefs;  // what Thermal setup's rows lit last (empty: the browser's rows)
  std::vector<opad::json> m_glyphFans;                       // the fans drawn with their plane and way
  class Job* m_profileJob = nullptr;                         // the sketches' regions found for a fan's opening
  std::map<std::string, opad::json> m_openings;              // a drawn opening's outline, centre and normal, by its reference (read on a worker)
  std::set<std::string> m_reading;                           // openings being read
  unsigned long long m_openingsRevision = ~0ull;             // the document's revision they were read at
  void showProfiles(bool on);                                // they offered in the view to pick (Thermal setup), or not
  void drawOpening(int fan, bool inlet);                     // a sketch made on the fan's face, its region the opening
  std::vector<std::pair<opad::Vec3, QWidget*>> m_badges;
  void showMade();  // the document's checks on the start page
  void leaveStart();  // a raw tool used on the Start tab: the Advanced tab, the view back in the start page's place
  SimulateStart* m_start = nullptr;
  GuidedCheck* m_check = nullptr;
  ToolPanel* m_checkPanel = nullptr;
  opad::json m_engines = opad::json::object();
  QPointer<Job> m_enginesJob;

  SimulatePanel* m_form = nullptr;
  bool m_modeShown = false;
  ToolPanel* m_panel = nullptr;
  ToolPanel* m_coolingPanel = nullptr;  // Thermal setup's
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
