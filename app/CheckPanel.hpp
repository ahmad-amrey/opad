#pragma once
// The results of a design check (TODO 10 B13 print check, B17 interference) in the "tool" panel: its options, a Check
// button and one row per finding. The window runs the check on a worker and highlights what a clicked row names.
#include <QWidget>

#include "opad/json.hpp"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;

class CheckPanel : public QWidget {
  Q_OBJECT
 public:
  enum class Mode { Interference, Print };
  explicit CheckPanel(QWidget* parent = nullptr);
  void begin(Mode mode);  // the mode's options, an empty list
  Mode mode() const { return m_mode; }
  opad::json options() const;  // interference: clearance_mm; print: build_direction, overhang_deg, min_wall_mm
  void setRunning(const QString& status);
  void setResult(const opad::json& result);  // check_interference / check_print output
  void setFailed(const QString& error);
  QSize preferredSize(int width) const;
  int findingCount() const { return static_cast<int>(m_findings.size()); }
  void activate(int row);  // benches: as if the row were clicked
 signals:
  void runRequested();
  void findingActivated(const opad::json& finding);  // interference: a pair; print: {body, faces, kind}
  void contentResized();  // the findings list came or went: the panel fits again
 private:
  void showFindings();   // the list only when it has rows
  Mode m_mode = Mode::Interference;
  QWidget* m_interference = nullptr;
  QDoubleSpinBox* m_clearance = nullptr;
  QWidget* m_print = nullptr;
  QComboBox* m_direction = nullptr;
  QDoubleSpinBox* m_overhang = nullptr;
  QDoubleSpinBox* m_minWall = nullptr;
  QPushButton* m_run = nullptr;
  QLabel* m_status = nullptr;
  QListWidget* m_list = nullptr;
  std::vector<opad::json> m_findings;
};
