#pragma once
// The Simulate workspace's panel (SimulateArea): the mechanism (its joints, each one's value on a slider), a new joint or
// relation from what is picked, and the studies: run one, step or play its frames, plot its series, show a structural
// study's result map. It only shows and asks; the area does the work.
#include <QString>
#include <QStringList>
#include <QWidget>
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
  void playRequested();
  void frameChosen(int frame);
  void seriesChosen(int index);
  void fieldChosen(const QString& field);
  void resultsToggled(bool shown);

 private:
  void jointKindChanged();
  QLabel *m_mechanism, *m_jointUnit, *m_relationLabel, *m_summary, *m_time, *m_jointHint;
  QComboBox *m_joints, *m_kind, *m_studies, *m_newKind, *m_series, *m_fields;
  QSlider *m_jointSlider, *m_frames;
  QDoubleSpinBox *m_jointValue, *m_relation;
  QPushButton *m_addJoint, *m_new, *m_run;
  QToolButton *m_play, *m_results;
  QWidget *m_motionRows, *m_resultRows;
  SimPlot* m_plot;
  PanelFooter* m_footer;
  double m_lo = 0, m_hi = 1;
};
