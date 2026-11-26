#pragma once
// A Simulate check taken step by step (SimChecks): the content of the "simcheck" ToolPanel, on the guided frame
// (GuidedSteps) Thermal setup uses too. Its steps follow the check's goal: the part, the inputs (where it is held and what
// pushes on it; what gets hot and how it is cooled; the joint and how far it moves; where the air goes in and out), how
// much / how hot / how hard, Run, Results. OPAD proposes what it can (the faces resting on something as supports, the top
// faces for the load, a duct's open ends, the parts named like chips as heat sources, the first joint to drive), read on a
// worker, and asks only for the rest.
//
// What it sets up is ordinary: one study (settings.check names the check, settings.guided keeps the panel's choices, case
// = the study's name) and the loads of that case, written a moment after each change (Debounce) as one step to undo, so
// the browser's Simulation folder, the Advanced tools and an AI agent see the same. A check whose solver is missing is set
// up all the same: only its Run is greyed (the solver window says where to get it). A check whose study kind this build
// does not have yet (the airflow checks before the flow study lands) can be filled in but writes nothing.
//
// Results lead with a verdict in plain words ("Safe: it holds 3.2 times this load"), then the numbers, the colour maps,
// and a Compare with another run of the same check (a copy made before a design change).
#include <QPointer>
#include <QWidget>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "opad/sim/study.hpp"

class AppDocument;
class GuidedSteps;
class Debounce;
class Job;
class JobRunner;
class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QGridLayout;
class QLabel;
class QPushButton;
class QVBoxLayout;
namespace simchecks {
struct Check;
}

class GuidedCheck : public QWidget {
  Q_OBJECT
 public:
  struct Hooks {
    std::function<AppDocument*()> document;
    std::function<opad::json(const std::string&, const opad::json&, const QString&)> write;  // a command, undoable; throws
    std::function<void(const std::string&)> run;  // runs the study in the background (Simulate::runStudy): runFinished follows
    std::function<bool()> busy;                   // a study is running
    std::function<std::shared_ptr<const opad::sim::StudyRun>(const std::string&)> lastRun;  // that study's run in memory, if any
    std::function<void(const QString&)> showField;     // the result map with that field ("von_mises", "temperature", "mode:0")
    std::function<void(const QString&)> command;       // runs an app command (simulate.play, simulate.cooling, inspect.material)
    std::function<void(const QString&)> solverNeeded;  // the solver window for that solver
    std::function<void(const QString&)> installSolver;  // installs OPAD's pack of it (Simulate::installSolver)
    std::function<bool(const QString&)> installing;     // that pack is being installed now
    std::function<opad::json()> engines;               // sim::engines() as last read (on a worker)
    std::function<QString(const QString&)> filter;     // the view's selection filter set to that action (empty: kept); the one before
    std::function<void(bool)> accumulate;              // the view's clicks add and take out (picking) or select
    std::function<std::vector<opad::Ref>()> selection;  // the refs selected now
    std::function<void(const std::vector<opad::Ref>&)> select;
    std::function<void(const std::vector<std::pair<opad::Ref, std::string>>&)> light;  // lit by nature ("heat", "fixed", "force", "fan")
    std::function<JobRunner*()> jobs;
  };
  GuidedCheck(Hooks hooks, QWidget* parent = nullptr);
  ~GuidedCheck() override;

  // Shows that check: `study` an existing one of it (its choices read back), empty a new one (written at the first change).
  void open(const QString& check, const std::string& study = {});
  QString check() const { return m_checkId; }
  std::string study() const { return m_study; }
  GuidedSteps* frame() const { return m_frame; }
  void escape();  // picking stops, else the panel's owner closes it (returns false: nothing to do here)
  bool picking() const { return m_picking >= 0; }
  void selectionChanged(const std::vector<opad::Ref>& refs);  // while picking: the list being picked becomes these
  void runFinished(const std::string& study, std::shared_ptr<const opad::sim::StudyRun> run, const QString& error);
  void enginesChanged();  // the solvers found again: Run greyed or not
  void showStep(const QString& which);  // "run", "results", "compare"
  bool canRun() const;
  bool hasResult() const;
  void runNow();     // the check written, then run (greyed: nothing)
  void flush();      // what is still to write written now

  // For benches.
  QString verdict() const;              // the verdict's headline ("Safe: ..."), empty before a result
  QStringList stepNames() const;
  std::vector<opad::Ref> listed(const QString& list) const;  // "supports", "load", "inlet", "outlet", "heat", "bodies"
  bool proposed(const QString& list) const;  // OPAD found something to propose for that list
  opad::json plan() const { return m_plan; }  // a flow check's mesh plan ({mesh, cells_estimate, inlet, outlet, ...}); null: none yet
  QString planError() const { return m_planError; }
  void useProposal(const QString& list);
  QPushButton* runButton() const { return m_runButton; }
  QLabel* runNote() const { return m_runNote; }
  // "weight", "need", "limit", "flow", "travel", "watts" (the first heat row), "freeFlow" and "shutoff" (a fan's label, CFM and
  // Pa), "quick" (1: Quick accuracy)
  void setValue(const QString& field, double v);
  // A check's result in one line ("Safe: holds 3.2 times this load"), from a study's summary and its settings.guided; empty
  // when it has none (not run).
  static QString verdictLine(const QString& check, const opad::json& summary, const opad::json& guided);

 protected:
  void hideEvent(class QHideEvent* e) override;
  void showEvent(class QShowEvent* e) override;

 signals:
  void closeRequested();
  void changed();  // its study, its result or its readiness changed (the ribbon's Run / Results follow)

 private:
  struct Setup;
  struct PickList;
  void build();          // the steps for the check shown
  void reload();         // the choices from the study (or a new check's defaults)
  void scheduleWrite();  // a change: written a moment later
  void write();          // the study and its loads as the choices say (one step to undo)
  void stepShown(int i);
  void propose();        // OPAD's proposals for the lists it can fill, read on a worker
  void startPicking(int list);
  void stopPicking();
  void showList(int list);
  void lightLists();
  void showRunState();
  void showResults();
  void showCompare();
  void keepCopy();  // a copy of this check to run after a design change; this one keeps its result
  QString bodyNames(const std::vector<std::string>& ids, int most = 3) const;
  std::vector<std::string> shownSolids() const;
  int stepOf(const QString& name) const;
  opad::json summary() const;  // the result shown: the run's, else the study's last
  void validateFlow(const opad::json& settings);  // a flow study's mesh plan from the core (a worker)
  void showPlan();
  opad::json m_plan;      // a flow check's mesh plan, as the core gave it
  QString m_planError;    // why the core refuses its settings or openings
  QPointer<Job> m_validating;
  QLabel *m_planText = nullptr, *m_runPlan = nullptr;

  Hooks m_hooks;
  QString m_checkId;
  const simchecks::Check* m_check = nullptr;
  std::string m_study;
  std::unique_ptr<Setup> m_setup;
  std::vector<std::unique_ptr<PickList>> m_lists;
  int m_picking = -1;
  QString m_filterBefore;
  QVBoxLayout* m_outer = nullptr;
  GuidedSteps* m_frame = nullptr;
  QStringList m_stepIds;  // "part", "held", "load", "print", "shake", "heat", "cool", "motion", "clear", "forces", "ends", "air", "run", "results"
  Debounce* m_write = nullptr;
  bool m_loading = false;   // fields being filled: their signals are not the user's
  bool m_writing = false;   // its own writes: the document's change is no reason to reload
  std::string m_written;    // what was written last (the same again writes nothing)
  unsigned long long m_proposedAt = ~0ull;  // the document revision the proposals were read at
  std::string m_proposedFor;                // and the bodies
  QPointer<Job> m_proposing;
  std::shared_ptr<const opad::sim::StudyRun> m_run;  // the run shown, while it is this study's
  // Run and Results.
  QLabel *m_estimate = nullptr, *m_runNote = nullptr, *m_status = nullptr, *m_writeNote = nullptr;
  QPushButton *m_runButton = nullptr, *m_solverButton = nullptr, *m_cooling = nullptr;
  QFrame* m_verdictCard = nullptr;
  QLabel *m_verdictHead = nullptr, *m_verdictBig = nullptr, *m_verdictText = nullptr;
  QGridLayout* m_numbers = nullptr;
  QWidget* m_numbersBox = nullptr;
  QWidget* m_fieldButtons = nullptr;
  QComboBox* m_compareWith = nullptr;
  QLabel* m_compareText = nullptr;
  QGridLayout* m_compareGrid = nullptr;
  QWidget* m_compareBox = nullptr;
  QString m_verdict;
  std::vector<std::function<void()>> m_refreshers;  // each page's fields filled from the setup again
};
