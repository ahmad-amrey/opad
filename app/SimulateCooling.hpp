#pragma once
// The Cooling assistant (simulate.cooling): a board, its chips and a heatsink in a vented box, cooled by fans or by warm air
// rising, taken step by step: the box (found by itself), the heat each part makes and which part is a board, how the air
// moves (fans, each on a block standing for it or on a heatsink, or vents only), the run (the air solved with OpenFOAM, the
// parts with CalculiX: sim/cfd.hpp) with its results, then the best place for the vents: a design parameter swept over a
// range (sim/sweep.cpp), the coolest design found and applied. What it sets up is ordinary loads (case "Cooling") and
// studies, there to change in the Simulation panel afterwards; an AI agent sees the same.
#include <QPointer>
#include <QWidget>

#include <functional>
#include <memory>
#include <string>

#include "opad/sim/study.hpp"

class AppDocument;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTableWidget;

class CoolingAssistant : public QWidget {
  Q_OBJECT
 public:
  struct Hooks {
    std::function<AppDocument*()> document;
    std::function<opad::json(const std::string&, const opad::json&, const QString&)> write;  // a command on the document, undoable
    std::function<void(const std::string&)> run;           // runs a study in the background (the Simulation panel shows it)
    std::function<bool()> busy;                            // a study is running
    std::function<void(const QString&)> showField;         // the result map with that field (temperature, air_speed)
    std::function<void(const std::string&, bool)> setVisible;  // a body shown or hidden
  };
  CoolingAssistant(Hooks hooks, QWidget* parent = nullptr);

  void open(int step = -1);  // shown and raised, at that step (-1: where it was)
  int step() const;
  int steps() const;
  // A study finished (or failed: run null): the results onto the Run or Optimise page.
  void runFinished(const std::string& study, std::shared_ptr<const opad::sim::StudyRun> run, const QString& error);

  // The names it gives what it makes.
  static constexpr const char* kCase = "Cooling";
  static QString studyName();
  static QString sweepName();

  // For benches.
  QComboBox* boxChoice() const { return m_box; }
  QTableWidget* heatTable() const { return m_heat; }
  QTableWidget* fanTable() const { return m_fans; }
  QComboBox* paramChoice() const { return m_param; }
  QTableWidget* sweepTable() const { return m_points; }
  QLabel* resultText() const { return m_result; }
  QPushButton* runButton() const { return m_runButton; }
  QPushButton* sweepButton() const { return m_sweepButton; }
  QPushButton* bestButton() const { return m_best; }
  bool apply();         // writes the loads and the study; false when something is missing (said on the page)
  bool startRun();      // apply, then run the study
  bool startSweep();    // the sweep over the chosen parameter
  void buildExample();  // the example board in its vented box, into an empty document

 protected:
  void keyPressEvent(class QKeyEvent* e) override;

 private:
  void reload();      // the pages from the document as it is now
  void boxChanged();  // the parts inside onto the Heat and Air pages
  void showStep(int i);
  void showSweep(const opad::json& summary);
  void addFanRow(const std::string& body, const opad::json& fan, const opad::Vec3& way);
  std::string studyId() const;
  std::string sweepId() const;

  Hooks m_hooks;
  QListWidget* m_steps = nullptr;
  QStackedWidget* m_pages = nullptr;
  QLabel* m_engine = nullptr;
  QComboBox* m_box = nullptr;
  QListWidget* m_inside = nullptr;
  QPushButton* m_example = nullptr;
  QTableWidget* m_heat = nullptr;
  QCheckBox* m_withFans = nullptr;
  QTableWidget* m_fans = nullptr;
  QDoubleSpinBox* m_ambient = nullptr;
  QComboBox* m_quality = nullptr;
  QCheckBox* m_radiation = nullptr;
  QPushButton* m_runButton = nullptr;
  QLabel* m_status = nullptr;
  QLabel* m_result = nullptr;
  QWidget* m_resultButtons = nullptr;
  QComboBox* m_param = nullptr;
  QDoubleSpinBox* m_from = nullptr;
  QDoubleSpinBox* m_to = nullptr;
  QSpinBox* m_count = nullptr;
  QSpinBox* m_refine = nullptr;
  QComboBox* m_objective = nullptr;
  QCheckBox* m_screen = nullptr;
  QPushButton* m_sweepButton = nullptr;
  QLabel* m_sweepStatus = nullptr;
  QTableWidget* m_points = nullptr;
  QPushButton* m_best = nullptr;
  opad::json m_bestParams;
  std::vector<std::string> m_candidates;  // the box choice's bodies
  std::vector<std::string> m_parts;       // the parts inside the box chosen
};
