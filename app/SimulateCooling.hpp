#pragma once
// Thermal setup (simulate.cooling, the CoolingAssistant): the guided setup of a thermal study of a board, its chips and a heatsink in a
// vented box, cooled by fans or by warm air rising, step by step: the box (its bodies picked: a base and its lid, a frame and
// its panels; found by itself), the heat each part makes (a body, or a face picked: a chip joined into its board) and which part
// is a board, how the air moves (fans, each on a block
// standing for it or on a heatsink, or vents only), and the run (the air solved with OpenFOAM, the parts with CalculiX:
// sim/cfd.hpp) with its results. What it sets up is ordinary loads (case "Cooling") and a study, there to change in the
// Simulation panel afterwards; an AI agent sees the same.
#include <QPointer>
#include <QWidget>

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
    std::function<JobRunner*()> jobs;                      // where the bodies' boxes are measured (a worker)
    std::function<void(const std::vector<opad::Ref>&)> select;  // bodies or faces selected in the view and the browser
    std::function<void(bool)> accumulate;                  // the view's clicks add and take out (picking) or select
    std::function<QString(const QString&)> filter;         // the view's selection filter set to that action (empty: kept); the one before
    std::function<bool()> filterSwitching;                 // a filter switch is being applied (its cleared selection is no pick)
    std::function<void(QObject*, std::function<void()>)> onFilterApplied;  // each time the view's filter has built its pick targets
  };
  CoolingAssistant(Hooks hooks, QWidget* parent = nullptr);

  void open(int step = -1);  // shown and raised, at that step (-1: where it was)
  int step() const;
  int steps() const;
  // A study finished (or failed: run null): its results onto the Run page.
  void runFinished(const std::string& study, std::shared_ptr<const opad::sim::StudyRun> run, const QString& error);

  // The names it gives what it makes.
  static constexpr const char* kCase = "Cooling";
  static QString studyName();

  // For benches.
  QListWidget* wallChoice() const { return m_walls; }  // the box's bodies picked, by name
  const std::vector<std::string>& pickedBodies() const { return m_picked; }
  bool picking() const { return m_picking != Pick::None; }
  bool pickingFaces() const { return m_picking == Pick::Faces; }
  const std::vector<opad::Ref>& heatFaces() const { return m_faces; }  // the faces that make heat, picked on the Heat page
  // The app's selection changed (the view, the browser): while the box's page picks, its bodies are what is selected; while
  // the Heat page picks, the faces that make heat are.
  void selectionChanged(const std::vector<std::string>& ids, const std::vector<opad::Ref>& refs);
  QTableWidget* heatTable() const { return m_heat; }
  QTableWidget* fanTable() const { return m_fans; }
  QLabel* resultText() const { return m_result; }
  QPushButton* runButton() const { return m_runButton; }
  bool apply();         // writes the loads and the study; false when something is missing (said on the page)
  bool startRun();      // apply, then run the study
  void buildExample();  // the example board in its vented box, into an empty document

 protected:
  void keyPressEvent(class QKeyEvent* e) override;
  void hideEvent(class QHideEvent* e) override;

 private:
  void reload();      // the pages from the document as it is now
  void showStep(int i);
  void addFanRow(const std::string& body, const opad::json& fan, const opad::Vec3& way);
  std::vector<std::string> walls() const;  // the box's bodies picked
  enum class Pick { None, Box, Faces };
  void startPicking(Pick what);  // the view and the browser pick the box's bodies, or the faces that make heat (their page)
  void stopPicking();
  void reselect();      // what is picked, shown selected
  void showPicked();    // the pick box and the list say what is picked
  void keepFacePowers();  // the powers typed for faces, kept while their rows are made again
  void showFaces();     // the faces that make heat: rows of the heat table after the bodies
  void wallsChanged();                     // what is inside them, and the heat and fan tables for it
  std::string studyId() const;

  Hooks m_hooks;
  QListWidget* m_steps = nullptr;
  QStackedWidget* m_pages = nullptr;
  QLabel* m_engine = nullptr;
  class PickBox* m_pick = nullptr;  // the box's bodies, picked in the view or the browser
  QListWidget* m_walls = nullptr;   // their names
  std::vector<std::string> m_picked;
  Pick m_picking = Pick::None;
  QString m_filterBefore;     // the view's filter before picking
  bool m_awaiting = false;    // the filter switched: the picks shown again once its targets are built
  QString pickFilter() const;  // the filter the page picks with
  void filterApplied();
  class PickBox* m_facePick = nullptr;   // the faces that make heat (a chip joined into its board has no body of its own)
  std::vector<opad::Ref> m_faces;
  std::map<std::string, double> m_facePower;  // by Ref::str
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
  std::vector<std::string> m_parts;       // the parts inside the box chosen
  // Every shown solid's box, measured on a worker as the assistant opens (reload): the box's bodies are found from them and
  // what is inside follows each tick from them, nothing measured on the UI thread.
  std::shared_ptr<const opad::sim::BodyBoxes> m_boxes;
  Job* m_job = nullptr;
  unsigned m_serial = 0;
  std::pair<unsigned long long, unsigned long long> m_loaded{~0ull, ~0ull};  // the document's generation and revision last loaded
 public:
  bool settled() const { return !m_job; }  // benches: the box's bodies found
};
