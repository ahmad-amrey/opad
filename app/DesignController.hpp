#pragma once
// The design flows behind the Design workspace: feature panels with live preview, sketch mode, parameters.
// Every change to the design goes through applyOps(): planned on a worker (the kernel never runs on the UI
// thread), committed on the UI thread as one undo step.
#include <QObject>
#include <QTimer>
#include <functional>

#include "AppDocument.hpp"
#include "DesignPanels.hpp"
#include "Jobs.hpp"
#include "Panels.hpp"
#include "SketchEditor.hpp"
#include "SketchPanel.hpp"
#include "Viewport.hpp"

class DesignController : public QObject {
  Q_OBJECT
 public:
  DesignController(AppDocument* doc, Viewport* viewport, JobRunner* jobs, QWidget* window);
  FeaturePanel* featurePanel() const { return m_form; }
  SketchEditor* sketch() const { return m_sketch; }
  void setPanel(ToolPanel* panel, std::function<void(ToolPanel*)> open);  // the floating panel the form lives in
  void setSketchPanel(ToolPanel* panel);
  void showSketchPanel();
  void redefineSketchPlane();

  void startFeature(const QString& kind);
  void editOp(const std::string& opId);  // a feature or a sketch, rolled back to when it was made
  void pickSketchPlane(std::function<void(opad::json,opad::Frame)> done);
  void startSketch();                    // asks for the plane first
  void finishSketch(std::function<void()> then = {});
  void cancelSketch();
  void showParameters();
  ParametersDialog* parametersWidget();
  void setParametersPanel(ToolPanel* panel) { m_parametersPanel=panel; }
  void regenerate(bool force);
  void setSuppressed(const std::string& featureId, bool on);
  // Plans `ops` on a worker and commits them. `done(ok, error)` runs on the UI thread.
  void applyOps(std::vector<opad::json> ops, const QString& label, std::function<void(bool, const QString&)> done = {});

  bool featureActive() const { return m_form->spec() != nullptr && m_featureOn; }
  bool pickingPlane() const { return m_pickPlane; }
  bool sketchActive() const { return m_sketch->active(); }
  bool busy() const { return m_doc->designBusy; }
  bool ownsSelection() const { return featureActive() || m_pickPlane; }  // the viewport's picks belong to a design input
  void viewportSelectionChanged();
  bool escape();  // Esc: leaves the plane pick or the feature; false when there was nothing to leave
  void bench();   // OPAD_BENCH_DESIGN

 signals:
  void stateChanged();                  // what is active changed: actions, ribbon
  void status(const QString& text);
  void failed(const QString& error);    // an apply that had no `done` of its own

 private:
  void beginPlanePick();
  void endFeature();
  void activateInput(const QString& name);
  void showCandidatesFor(const QString& type);
  void syncSelectionToInput();
  void schedulePreview();
  void runPreview(bool commit);
  void enterSketch(const std::string& sketchId, const QString& name, const opad::json& plane, const opad::Frame& frame, const opad::json& geometry);
  opad::json pickToJson(const opad::Ref& ref) const;

  AppDocument* m_doc;
  Viewport* m_viewport;
  JobRunner* m_jobs;
  QWidget* m_window;
  FeaturePanel* m_form;
  ToolPanel* m_panel = nullptr;
  ToolPanel* m_sketchPanel = nullptr;
  bool m_replaning = false;
  std::function<void(ToolPanel*)> m_openPanel;
  SketchEditor* m_sketch;
  ParametersDialog* m_params = nullptr;
  ToolPanel* m_parametersPanel = nullptr;

  bool m_featureOn = false;
  std::string m_editing;        // feature op being edited (empty: a new one)
  std::string m_newId;          // id the new feature's op will get (so previews can be matched to it)
  std::function<void(opad::json,opad::Frame)> m_planePicked;
  bool m_pickPlane = false;
  bool m_activating = false;    // the selection is being re-applied for the newly active input: not a pick
  Viewport::SelFilter m_filterBefore = Viewport::SelFilter::Body;
  QTimer m_previewTimer;
  Job* m_planJob = nullptr;
  Job* m_candidateJob = nullptr;
  Job* m_planeJob=nullptr;int m_planeSerial=0;
  int m_planSerial = 0;
  std::shared_ptr<opad::design::Plan> m_readyPlan;  // computed for m_readyInputs on m_readyOps ops
  std::string m_readyInputs;
  size_t m_readyOps = 0;
};
