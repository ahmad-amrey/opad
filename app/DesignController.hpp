#pragma once
// The design flows behind the Design workspace: feature panels with live preview, sketch mode, parameters.
// Every change to the design goes through applyOps(): planned on a worker (the kernel never runs on the UI
// thread), committed on the UI thread as one undo step.
#include <QObject>
#include <QTimer>
#include <gp_Ax1.hxx>
#include <array>
#include <functional>
#include <memory>
#include <set>
#include <tuple>

#include "AppDocument.hpp"
#include "DesignPanels.hpp"
#include "Jobs.hpp"
#include "Panels.hpp"
#include "SketchEditor.hpp"
#include "SketchPanel.hpp"
#include "Viewport.hpp"
#include "PlanePicker.hpp"

class PrimitivePlacer;
class ToolValues;
class TranslateTriad;

class DesignController : public QObject {
  Q_OBJECT
 public:
  DesignController(AppDocument* doc, Viewport* viewport, JobRunner* jobs, QWidget* window);
  ~DesignController() override;
  FeaturePanel* featurePanel() const { return m_form; }
  SketchEditor* sketch() const { return m_sketch; }
  // The feature's values typed from the keyboard (UI-122): its boxes beside the pointer, and the extrude's by the arrow.
  ToolValues* values() const { return m_values; }
  DimensionHandle* distanceHandle() const;
  // The existing sketch or feature op an open editor changes (empty while nothing is edited, or for a new one).
  std::string editingOp() const;
  void setPanel(ToolPanel* panel, std::function<void(ToolPanel*)> open);  // the floating panel the form lives in
  void setSketchPanel(ToolPanel* panel);
  // The component selected in the browser (empty: none): where a new feature's bodies go unless its panel says else.
  void setCurrentComponent(std::function<std::string()> current) { m_currentComponent = std::move(current); }
  void showSketchPanel(const QString& page = {});  // page: the title of the page shown instead of the tool
  void redefineSketchPlane();
  // A new sketch on a plane known already (Drafting's Draw on drawing: the drawing's own frame), without picking one.
  void startSketchOn(const opad::json& plane, const opad::Frame& frame);

  void startFeature(const QString& kind);
  // A new feature with inputs given (picks as their JSON array, values and flags as typed): Paste's Move / copy (UI-129).
  void startFeature(const QString& kind, const std::vector<std::pair<QString, opad::json>>& given);
  void editOp(const std::string& opId);  // a feature or a sketch, rolled back to when it was made
  // `command`: what the plane is chosen for (its help, the picker's title): New sketch, Align view to plane, Import's drawing.
  void pickSketchPlane(std::function<void(opad::json,opad::Frame)> done,bool positionOrigin=false,const QString& command="design.sketch");
  ToolPanel* planePanel() const { return m_planePicker->panel(); }
  PlanePicker* planePicker() const { return m_planePicker; }
  // A new box, cylinder, cone, sphere, torus or coil placed in the view (TODO 11 P1): null-safe, always made.
  PrimitivePlacer* placer() const { return m_placer; }
  void startSketch();                    // asks for the plane first
  void finishSketch(std::function<void()> then = {});
  void cancelSketch();
  void showParameters();
  ParametersDialog* parametersWidget();
  void setParametersPanel(ToolPanel* panel) { m_parametersPanel=panel; }
  void regenerate(bool force);
  void setSuppressed(const std::string& featureId, bool on);
  // Plans `ops` on a worker and commits them. `done(ok, error)` runs on the UI thread. `extend` adds to the plan on the
  // worker (its own ops and bodies, against the same copy of the document).
  void applyOps(std::vector<opad::json> ops, const QString& label, std::function<void(bool, const QString&)> done = {},
                std::function<void(opad::Document&, opad::design::Plan&)> extend = {});
  // A plan made on a worker over a copy of the document as it is now (the caller compared the revision) committed as
  // applyOps commits its own, without planning again.
  void commitPlanned(std::shared_ptr<opad::design::Plan> plan, const QString& label, std::function<void(bool, const QString&)> done = {});

  bool featureActive() const { return m_form->spec() != nullptr && m_featureOn; }
  bool pickingPlane() const { return m_pickPlane; }
  QString planeCommand() const { return m_replaning ? QString("sketch.replane") : m_planeCommand; }  // the command a plane pick serves
  bool sketchActive() const { return m_sketch->active(); }
  bool busy() const { return m_doc->designBusy; }
  bool previewing() const { return m_planJob || m_previewTimer.isActive() || m_previewPending; }  // a preview being planned (benches: wait for it)
  bool ownsSelection() const { return featureActive() || m_pickPlane; }  // the viewport's picks belong to a design input
  void viewportSelectionChanged();
  bool escape();  // Esc: leaves the plane pick or the feature; false when there was nothing to leave
  bool undoPick();  // Ctrl+Z in a feature's panel: takes back the last pick; false (and says why) when there is none
  void bench();   // OPAD_BENCH_DESIGN
  void benchSketch(std::function<void()> run);  // Sketch1 on XY opened, then run (a bench)
  opad::json recoveryState() const;
  void restoreRecovery(const opad::json& state);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;  // Enter in the view accepts the open feature

 signals:
  void stateChanged();                  // what is active changed: actions, ribbon
  void status(const QString& text);     // what the tool waits for (the status bar's prompt), or why a change was refused
  void notice(const QString& text);     // a result or a hint that goes by itself (a toast)
  void failed(const QString& error);    // an apply that had no `done` of its own

 private:
  void beginPlanePick();
  void endFeature();
  void activateInput(const QString& name);
  void showCandidatesFor(const QString& type, bool planes = false);
  std::vector<Viewport::Candidate> quickCandidates(const std::string& type) const;  // origin and construction axes or planes
  // Sketch regions, points, lines or paths of the scene's sketches, on a worker, added to `found`; then `done(ok)`.
  Job* sketchCandidates(const std::string& type, std::shared_ptr<std::vector<Viewport::Candidate>> found, std::function<void(bool)> done);
  void showAllCandidates();  // the active input's candidates and the routed ones
  // Pick routing (TODO 11 P3): while a selection input with a pick is active, the axes or planes of the next axis or plane
  // input that is still empty, on its default or filled this way show beside its own candidates, and a click on one goes
  // there (Revolve: the profile, then the Z axis; Mirror: the body, then the YZ plane). With no input active, the feature's
  // plane input takes a click on a planar face, an origin plane or a construction plane (a construction plane's face).
  QString routeTarget() const;
  void refreshRoute();
  bool routeClick();  // a click on a routed candidate: into its input (true), else false
  bool placedPlane(const opad::design::InputSpec& in) const;  // a primitive's plane: placed with its position, not by a click here
  QString idlePlaneInput() const;
  void idlePlaneClick();
  void pickIdlePlane(const QString& input, opad::json support);  // a face resolved on a worker first
  void inputsSettled();  // an input changed: routing and the idle plane input follow
  void resetRouting();   // a feature begins or ends: nothing routed, no idle plane input
  void pickStatus();     // the prompt: what the active input waits for, and the routed input's click
  void refreshPlanCopies();  // m_planDoc / m_planScene for the document as it is now
  double modelReach() const;
  void syncSelectionToInput();
  void schedulePreview();
  void runPreview(bool commit);
  void commitDerived(const opad::json& inputs, const std::string& component, const QString& label);  // Sketch on face
  QList<DynamicInput::Field> valueFields(const QString& except = {}) const;  // the panel's values shown, as boxes
  void typeValue(const QString& key, QString value);  // typed into a box: into the panel (the preview follows)
  void refreshValues();                                // the boxes follow the panel
  void enterSketch(const std::string& sketchId, const QString& name, const opad::json& plane, const opad::Frame& frame, const opad::json& geometry);
  opad::json pickToJson(const opad::Ref& ref) const;

  AppDocument* m_doc;
  Viewport* m_viewport;
  JobRunner* m_jobs;
  QWidget* m_window;
  FeaturePanel* m_form;
  ToolPanel* m_panel = nullptr;
  ToolPanel* m_sketchPanel = nullptr;
  bool m_replaning = false,m_positionOrigin=false;
  PlanePicker* m_planePicker;
  std::function<void(ToolPanel*)> m_openPanel;
  std::function<std::string()> m_currentComponent;
  // What a rule chosen with "By rule…" matched, by input, to show it as the selection (TODO 10 B7).
  std::map<QString, std::vector<opad::Ref>> m_ruleMatches;
  void offerRules(const QString& input, QWidget* anchor);
  SketchEditor* m_sketch;
  ParametersDialog* m_params = nullptr;
  ToolPanel* m_parametersPanel = nullptr;

  bool m_featureOn = false;
  std::string m_editing;        // feature op being edited (empty: a new one)
  opad::json m_editResult;      // what that feature made when editing began: the preview until an input changes it
  bool m_previewPending = false;  // a handle drag changed the value while a preview plan was running
  std::string m_newId;          // id the new feature's op will get (so previews can be matched to it)
  std::function<void(opad::json,opad::Frame)> m_planePicked;
  bool m_pickPlane = false;
  QString m_planeCommand = "design.sketch";
  bool m_activating = false;   // the selection is being re-applied for the newly active input: not a pick
  Viewport::SelFilter m_filterBefore = Viewport::SelFilter::Body;
  QTimer m_previewTimer;
  QPointer<DimensionHandle> m_distanceHandle;  // the feature's value arrow (TODO 11 P2): extrude's distance, a fillet's radius, ...
  QString m_handleInput = "distance";          // the input it pulls
  void showHandle(const opad::json& handle);   // a feature_handles entry: the arrow there, bound to its input
  QList<DynamicInput::Field> handleExtras(const QString& input) const;  // the boxes after the arrow's: Tab goes round them
  PrimitivePlacer* m_placer = nullptr;
  // Move / copy's triad (TODO 11 P2, DesignTriad.cpp): X, Y and Z arrows and a square on the picked bodies, and with Rotate
  // on a ring round the move's axis; a pull sets the distances (rounded at this zoom) or the angle (5 degree steps) and the
  // preview follows as during the extrude's drag; the boxes by the pointer show the values, the pulled part's taking what is
  // typed next.
  std::unique_ptr<TranslateTriad> m_triad;
  struct TriadPull {
    int part = -1;                   // -1 none, 0 the square, 1-3 X, Y, Z, TranslateTriad::kRing the ring
    opad::Vec3 start{0, 0, 0};       // dx, dy, dz at the press
    double angle = 0;                // the angle at the press (radians)
  } m_pull;
  struct MoveAxis {
    bool valid = false;
    opad::json input;                // the axis input it was resolved for
    opad::Vec3 origin{0, 0, 0}, dir{0, 0, 1};
  } m_moveAxis;                      // a move's axis other than X, Y or Z, from the preview's worker
  void placeMoveTriad();
  bool triadEvent(QEvent* event);  // the viewport's mouse events; true: the triad took it
  bool pulling() const;            // a handle or the triad is being pulled: previews as fast as plans come
  double inputValue(const QString& name, opad::design::Dim dim, double fallback) const;  // a value input now (fallback: it does not evaluate)
  bool moveAxis(gp_Ax1& axis) const;  // the axis the move turns about, when known
  void moveAxisResolved(const opad::json& handles, const opad::json& inputs);
 public:
  // Benches (TODO 11 P2): Move's triad, the input the value arrow pulls while it shows (empty: none), the preview plan on
  // screen and the inputs it was made for.
  TranslateTriad* moveTriad() const { return m_triad.get(); }
  QString handleInput() const;
  std::shared_ptr<const opad::design::Plan> readyPreview() const { return m_readyPlan; }
  opad::json readyPreviewInputs() const { return m_readyPlan ? m_readyValues : opad::json(); }
 private:
  ToolValues* m_values = nullptr;
  Job* m_planJob = nullptr;
  Job* m_candidateJob = nullptr;
  std::vector<Viewport::Candidate> m_activeCandidates, m_routeCandidates;  // shown together (showAllCandidates)
  QString m_routeInput;            // the input a click on a routed candidate fills (empty: none shown)
  std::set<std::string> m_routeIds;  // the routed candidates' ids
  std::set<QString> m_routed;      // inputs filled by a routed click: they stay the target, to be changed by another click
  Job* m_routeJob = nullptr;
  int m_routeSerial = 0;
  QString m_idlePlane;             // the plane input a click fills while no input is active
  Job* m_idleJob = nullptr;
  int m_idleSerial = 0;
  QString m_nothingToPick;      // the active input has no candidates at all: says so instead of "Pick: …"
  int m_planSerial = 0;
  std::shared_ptr<opad::design::Plan> m_readyPlan;  // computed for m_readyInputs on m_readyOps ops
  std::string m_readyInputs;
  opad::json m_readyValues;  // its inputs (benches)
  size_t m_readyOps = 0;
  // What preview plans read: copies of the document and the (rolled back) scene, made once per document state
  // instead of once per plan (on the Engine each copy cost the UI thread tens of ms per drag step).
  std::shared_ptr<const opad::Document> m_planDoc;
  std::shared_ptr<const opad::Scene> m_planScene;
  std::tuple<unsigned long long, unsigned long long, size_t> m_planStamp{};
  // The extrude handle's live stretch: the last exact preview and how to pull it along the axis while the next plan
  // runs, so the body follows the pointer at the frame rate whatever a plan costs.
 public:
  struct Stretch {
    bool valid = false, symmetric = false, footprint = false;
    std::array<double, 3> origin{}, axis{0, 0, 1}, u{1, 0, 0}, v{0, 1, 0};
    double from = 0, u0 = 0, u1 = 0, v0 = 0, v1 = 0;
    std::vector<std::shared_ptr<const BodyPrs>> base;  // the preview parts' arrays, in their order
  };
 private:
  Stretch m_stretch;
  void stretchPreview(double value);
};
