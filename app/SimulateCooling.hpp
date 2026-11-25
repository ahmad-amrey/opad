#pragma once
// Thermal setup (simulate.cooling, the CoolingAssistant), the content of the "cooling" ToolPanel, its steps as tabs: the guided
// setup of a thermal study of a board, its chips and a heatsink in a vented box, cooled by fans, by fans with the warm air rising
// too (mixed convection, slower), or by natural convection: the box (the bodies it is made of: a base and its lid, a frame and
// its panels), the parts and faces that make heat (a chip joined into its board is a face) with their power and which part is a
// board, how the air moves (fans, each by its openings: an axial fan's, a blower's inlet and outlet, sketch regions drawn on
// its faces, with its datasheet's airflow and pressure; drawn in the view with the air's way; the heatsink it blows through
// with its material; whether the box's vents let them blow what they can), and the run (the
// air solved with OpenFOAM, the parts with CalculiX: sim/cfd.hpp; Quick by default, with an estimate of how long) with its
// results. Every list starts empty (or as the study left it) and is filled by its Add button, which picks in the view and the
// browser while it is on: a click adds, never takes out (Remove does, for the rows chosen). What it sets up is ordinary loads
// (case "Cooling") and a study, written as it is set (each change one step to undo, the loads changed in place), so they show in
// the browser's Simulation folder as the manual commands' do, and can be changed in the Simulation panel afterwards; an AI agent
// sees the same.
#include <QPointer>
#include <QWidget>

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "opad/sim/cfd.hpp"
#include "opad/sim/study.hpp"

class AppDocument;
class Job;
class JobRunner;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QStackedWidget;
class QTableWidget;

class CoolingAssistant : public QWidget {
  Q_OBJECT
 public:
  struct Hooks {
    std::function<AppDocument*()> document;
    std::function<opad::json(const std::string&, const opad::json&, const QString&)> write;  // a command on the document, undoable
    std::function<void(const QString&, const std::function<void()>&)> batch;  // the writes inside as one step to undo (none: each its own)
    std::function<void(const std::string&)> run;           // runs a study in the background (the Simulation panel shows it)
    std::function<bool()> busy;                            // a study is running
    std::function<void(const QString&)> showField;         // the result map with that field (temperature, air_speed)
    std::function<void(const std::string&, bool)> setVisible;  // a body shown or hidden
    std::function<JobRunner*()> jobs;                      // where the fan's face is read (a worker)
    std::function<void(const std::vector<opad::Ref>&)> select;  // bodies or faces selected in the view and the browser
    // The rows chosen in a list lit in the view by what they are ("heat", "board", "box", "fan", "heatsink"): a face too,
    // whatever the selection filter; none: nothing lit.
    std::function<void(const std::vector<std::pair<opad::Ref, std::string>>&)> light;
    std::function<void(bool)> panel;  // its panel shown (as the other panels: the one shown) or hidden; none: a window of its own
    std::function<void(const std::vector<opad::json>&)> fanGlyphs;  // fans ({refs, vector, outlet}) drawn with their plane and way
    std::function<void(bool)> profiles;                      // the sketches' closed regions offered in the view to pick, or not
    std::function<void(const std::string&)> selectCandidate;  // a profile (its candidate id) shown selected
    std::function<void(int, bool)> draw;                      // a fan's outlet (or inlet) drawn: a sketch on a face, then setProfile
    std::function<void(bool)> accumulate;                  // the view's clicks add and take out (picking) or select
    std::function<QString(const QString&)> filter;         // the view's selection filter set to that action (empty: kept); the one before
    std::function<bool()> filterSwitching;                 // a filter switch is being applied (its cleared selection is no pick)
    std::function<void(QObject*, std::function<void()>)> onFilterApplied;  // each time the view's filter has built its pick targets
  };
  CoolingAssistant(Hooks hooks, QWidget* parent = nullptr);

  void open(int step = -1);  // its panel shown, at that step (-1: where it was)
  void dismiss();            // its panel closed (what is still to write written first)
  void escape();             // Esc: the Add button that picks goes off, else the panel closes
  int step() const;
  int steps() const;
  // A study finished (or failed: run null): its results onto the Run page.
  void runFinished(const std::string& study, std::shared_ptr<const opad::sim::StudyRun> run, const QString& error);

  // The names it gives what it makes.
  static constexpr const char* kCase = "Cooling";
  static QString studyName();

  // For benches.
  QListWidget* wallChoice() const { return m_walls; }  // the box's bodies, by name (their ids in Qt::UserRole)
  const std::vector<std::string>& pickedBodies() const { return m_picked; }
  bool picking() const { return m_picking != Pick::None; }
  void reselect();  // what the list being added to holds, shown selected
  bool pickingFaces() const { return m_picking == Pick::Faces; }
  std::vector<std::string> heatBodies() const;  // the heat table's parts (bodies), in its order
  std::vector<opad::Ref> heatFaces() const;     // its faces (a chip joined into its board)
  // The app's selection changed (the view, the browser): while an Add button is on, what was clicked is added to its list.
  void selectionChanged(const std::vector<std::string>& ids, const std::vector<opad::Ref>& refs, const std::vector<std::string>& candidates = {});
  // A fan's opening drawn by Draw… (a sketch made on its face), taken as that fan's outlet or inlet.
  void setProfile(int fan, bool inlet, const opad::json& profile);
  QTableWidget* heatTable() const { return m_heat; }
  bool naturalConvection() const;  // no fans: the air moves by rising where it is warm
  bool warmAirRises() const;       // natural convection, or fans and the air rising too (cfd.buoyancy: slower)
  QString runKind() const;         // its quality and how the air moves, as the last runs' durations are kept
  void showEstimate();             // how long Run would take, said before it starts
  // A fan as the Air page sets it up, by its openings (no body): an axial fan by its opening (a sketch's closed region drawn
  // on its face, or a flat face; the air through it, along its normal or flipped), a blower by its inlet (on its axis) and its
  // outlet (its side), its housing staying solid; and its datasheet: max airflow and max static pressure in the units the
  // datasheet gives, its P-Q curve read off the chart (optional), the speed it runs at. Written as a fan load (sim/cfd.hpp).
  struct FanSpec {
    QString type = "axial";           // "axial" or "blower"
    opad::json outlet, inlet;         // its openings ({sketch, at, boundary} or a face reference); null: not yet
    opad::Vec3 normal{0, 0, 0};       // the outlet's normal (fan_profile), read on a worker; 0: not yet
    opad::Vec3 centre{0, 0, 0}, inletCentre{0, 0, 0}, inletNormal{0, 0, 0};
    std::vector<std::vector<opad::Vec3>> outletLoops, inletLoops;  // their outlines, drawn in the view
    bool flip = false;                // an axial fan's air the other way
    bool flipOut = false, flipIn = false;  // a blower's openings turned from the way found (out of its housing)
    QString model;                    // a library fan's id; empty: from its datasheet (below)
    double flow = 1, pressure = 0.5;  // max airflow, max static pressure, in the units below
    QString flowUnit = "cfm", pressureUnit = "inH2O";
    std::vector<std::array<double, 2>> curve;  // its P-Q curve off the datasheet's chart, those units (optional)
    double speed = 100;               // % of its rated speed (PWM, a lower voltage)
    std::vector<std::string> sink;    // the heatsink it blows through: bodies or a component (one part)
    QString material;                 // the heatsink's library material, empty: as modelled
    std::vector<std::string> on;      // an older setup's fan body and way (kept as written until its outlet is drawn)
    opad::Vec3 written{1, 0, 0};      // and its vector
    opad::Vec3 vector() const;        // the way an axial fan's air goes (a blower's out of its outlet)
    opad::json spec() const;          // the load's "fan"
    bool drawing = false;             // Draw… is making one of its openings (setProfile takes it there)
    bool loaded = false;              // read from its load: Flip is found again from the way written once the opening is read
    bool drawn() const { return !outlet.is_null() && (type != "blower" || !inlet.is_null()); }
    bool held() const { return drawn() || (outlet.is_null() && !on.empty()); }  // a load holds it
  };
  const std::vector<FanSpec>& fans() const { return m_fanSpecs; }
  int currentFan() const { return m_fan; }
  QListWidget* fanList() const { return m_fanList; }
  QLabel* resultText() const { return m_result; }
  QPushButton* runButton() const { return m_runButton; }
  bool apply();         // the loads and the study written now; false when something is missing (said on the page)
  void sync();          // what the pages hold written now (as far as it goes: a fan not picked yet is no load); each change schedules it
  bool startRun();      // apply, then run the study
  void buildExample();  // the example board in its vented box, into an empty document

 protected:
  void keyPressEvent(class QKeyEvent* e) override;
  void hideEvent(class QHideEvent* e) override;

 private:
  void reload();      // the pages from the document as it is now
  void scheduleSync();  // a change of the pages: written a moment later (typing a power writes once)
  void showStep(int i);
  void lightChosen();  // the rows chosen on the page shown (the box's, the heat table's, the current fan) lit in the view
  std::vector<std::string> walls() const;  // the box's bodies
 public:
  // What the view and the browser pick: the box's bodies, the parts or faces that make heat, a fan, the face its air crosses,
  // its heatsink. Each page picks with one of them at a time; its pick boxes choose which.
  enum class Pick { None, Box, Parts, Faces, Outlet, Inlet, Sink };
  Pick pickingWhat() const { return m_picking; }
 private:
  void startPicking(Pick what);
  void stopPicking();
  void showPicked();    // the box's list
  void syncAdd();       // the Add buttons say whether they pick
  bool pickable(const std::string& id) const;  // a shown body of the document
  void addHeatBody(const std::string& id, double watts, bool board, int layers);
  void addHeatFace(const opad::Ref& face, double watts);
  void loadFans(const opad::Scene& s);  // the fans the study's loads set
  void showFans();      // the fan list and the current fan's form
  void showFan();       // the current fan's form
  void measureOutlet(int fan);  // the outlet's normal (a sketch-sized read)
  QString profileText(const opad::json& profile) const;  // "Blower outlet (sketch)", "Fan, face 3"
  std::string studyId() const;

  Hooks m_hooks;
  class GuidedSteps* m_frame = nullptr;  // the steps' frame, shared with Simulate's checks
  class QTabBar* m_steps = nullptr;  // its tabs: Box, Heat, Air, Run
  QLabel* m_engine = nullptr;
  QListWidget* m_walls = nullptr;   // the box's bodies, by name
  QPushButton* m_boxAdd = nullptr;  // on: clicks in the view and the browser add bodies to it
  QPushButton* m_boxRemove = nullptr;
  std::vector<std::string> m_picked;
  Pick m_picking = Pick::None;
  QString m_filterBefore;     // the view's filter before picking
  bool m_awaiting = false;    // the filter switched: the picks shown again once its targets are built
  QString pickFilter() const;  // the filter the page picks with
  void filterApplied();
  QPushButton* m_partAdd = nullptr;  // on: clicks add parts that make heat (bodies)
  QPushButton* m_faceAdd = nullptr;  // on: clicks add faces that make heat (a chip joined into its board)
  QPushButton* m_heatRemove = nullptr;
  QPushButton* m_example = nullptr;
  QTableWidget* m_heat = nullptr;  // a row per part or face that makes heat (or a board): its power, board, copper layers
  QRadioButton* m_fansOn = nullptr;   // fans move the air
  QRadioButton* m_natural = nullptr;  // natural convection: no fans, the air rises where it is warm
  QRadioButton* m_mixed = nullptr;    // fans, and the air rising where it is warm (mixed convection)
  QLabel* m_airNote = nullptr;        // what the choice includes and what it costs
  QString m_airChanged;               // why the choice shown is not the one made (the study changed outside the panel)
  std::function<void()> m_showAirNote;
  bool m_sameDocument = false;        // the last reload was of the document loaded before
  std::vector<FanSpec> m_fanSpecs;
  int m_fan = -1;
  bool m_fanLoading = false;  // the form being filled: its edits are not the user's
  QListWidget* m_fanList = nullptr;
  QWidget* m_fanForm = nullptr;
  class QFormLayout* m_fanRows = nullptr;
  QComboBox* m_fanType = nullptr;
  class PickBox* m_outletPick = nullptr;
  class PickBox* m_inletPick = nullptr;
  QPushButton* m_outletDraw = nullptr;
  QPushButton* m_inletDraw = nullptr;
  QPushButton* m_outletFlip = nullptr;  // a blower's: its outlet's way, its inlet's way turned round
  QPushButton* m_inletFlip = nullptr;
  class PickBox* m_sinkPick = nullptr;
  QPushButton* m_flip = nullptr;
  QLabel* m_wayText = nullptr;
  QComboBox* m_model = nullptr;
  QDoubleSpinBox* m_flow = nullptr;
  QComboBox* m_flowUnit = nullptr;
  QDoubleSpinBox* m_pressure = nullptr;
  QComboBox* m_pressureUnit = nullptr;
  class QLineEdit* m_curve = nullptr;  // the P-Q points
  QLabel* m_ventText = nullptr;        // whether the box's vents let the fans blow what they can (sim::vent_estimate)
  QCheckBox* m_ventsShown = nullptr;   // the vents it found drawn in the view (intakes in, exhausts out)
  opad::json m_ventOpenings;           // their openings, as the estimate lists them
  int m_ventRun = 0;                   // the estimate asked last (an older one's answer is dropped)
  QPointer<Job> m_ventJob;             // the one running (a new one cancels it: never several at once)
  std::string m_ventKey;               // what the last one was asked with: the same again (the Air tab shown again) runs nothing
  void estimateVents();
  QDoubleSpinBox* m_speed = nullptr;
  class ClipView* m_fanClip = nullptr;  // how its openings are drawn
  QComboBox* m_sinkMaterial = nullptr;

  QPushButton* m_back = nullptr;
  QPushButton* m_next = nullptr;
  QLabel* m_runNote = nullptr;  // why Run cannot run (no OpenFOAM)
  QDoubleSpinBox* m_ambient = nullptr;
  QComboBox* m_quality = nullptr;
  QLabel* m_estimate = nullptr;     // how long a run would take
  QCheckBox* m_radiation = nullptr;
  QPushButton* m_runButton = nullptr;
  QLabel* m_status = nullptr;
  QLabel* m_result = nullptr;
  QWidget* m_resultButtons = nullptr;
  std::pair<unsigned long long, unsigned long long> m_loaded{~0ull, ~0ull};  // the document's generation and revision last loaded
  class QTimer* m_syncTimer = nullptr;
  bool m_reloading = false;  // the pages being filled from the document: no change of the user's
  bool m_writing = false;    // its own writes: the document's change is no reason to reload
 public:
  bool settled() const { return true; }  // benches: loaded (nothing is measured any more: the lists start as the study left them)
  QPushButton* boxAdd() const { return m_boxAdd; }
  QPushButton* partAdd() const { return m_partAdd; }
  QPushButton* faceAdd() const { return m_faceAdd; }
};
