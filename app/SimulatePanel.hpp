#pragma once
// The Simulate workspace's panel (SimulateArea), the Advanced tools' (its bar at the top chooses, setMode): Mechanism has the mechanism (its joints,
// each one's value on a slider), a new joint or relation from what is picked, and its motion and dynamic studies; Structure
// its static and vibration studies; Thermal its thermal studies and the Thermal setup. Each runs a study, steps or plays its
// frames, plots its series, shows a result map. It only shows and asks; the area does the work.
#include <QString>
#include <QStringList>
#include <QWidget>
#include <string>
#include <utility>
#include <vector>

class PanelFooter;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QToolButton;
class SimPlot;

class SimulatePanel : public QWidget {
  Q_OBJECT
 public:
  using Entries = std::vector<std::pair<QString, QString>>;  // id, shown text
  explicit SimulatePanel(QWidget* parent = nullptr);
  enum class Mode { Mechanism, Structure, Thermal };
  void setMode(Mode mode);  // what the panel is about: its sections and the kinds of New study
  Mode mode() const { return m_mode; }
  static Mode modeOf(const std::string& studyKind);  // the tab a study's kind belongs to (a sweep: its own study's)
  // Mechanism
  void setMechanism(const QString& status);
  void setJoints(const Entries& joints, const QString& current);
  void setJointValue(bool enabled, double lo, double hi, double value, const QString& unit);
  QString jointKind() const;
  double relationValue() const;  // a relation's ratio, pitch radius or lead (0: from the gears' teeth)
  // Studies
  void setStudies(const Entries& studies, const QString& current);
  void setRun(const QString& summary, int frames, bool structural);
  void setFrame(int frame, const QString& time);
  void setSeries(const QStringList& names, int current);
  void setFields(const Entries& fields, const QString& current);
  void setPlaying(bool playing);
  void setResultsShown(bool shown);
  SimPlot* plot() const { return m_plot; }
  PanelFooter* footer() const { return m_footer; }

 signals:
  void jointChosen(const QString& id);
  void jointValueChanging(double value);
  void jointValueChosen(double value);
  void addJointRequested(const QString& kind);
  void studyChosen(const QString& id);
  void newStudyRequested(const QString& kind);
  void runRequested();
  void guideRequested();
  void coolingRequested();
  void playRequested();
  void frameChosen(int frame);
  void seriesChosen(int index);
  void fieldChosen(const QString& field);
  void resultsToggled(bool shown);
  void modeChosen(Mode mode);  // its Mechanism / Structure / Thermal bar

 private:
  void jointKindChanged();
  QLabel *m_mechanism, *m_jointUnit, *m_relationLabel, *m_summary, *m_time, *m_jointHint;
  QComboBox *m_joints, *m_kind, *m_studies, *m_newKind, *m_series, *m_fields;
  QSlider *m_jointSlider, *m_frames;
  QDoubleSpinBox *m_jointValue, *m_relation;
  QPushButton *m_addJoint, *m_new, *m_run, *m_cooling;
  QToolButton *m_play, *m_results;
  QWidget *m_motionRows, *m_resultRows, *m_mechanismBox;
  QLabel* m_studyHeader;
  Mode m_mode = Mode::Mechanism;
  class QTabBar* m_modes = nullptr;
  SimPlot* m_plot;
  PanelFooter* m_footer;
  double m_lo = 0, m_hi = 1;
};
