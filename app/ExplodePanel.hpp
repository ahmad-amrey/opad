#pragma once
// The Explode panel (UI-36): the exploded view's settings, edited live. How deep components split (the level control) and
// how far the parts have moved (the distance slider, with Play) are separate controls, never one slider cut into a
// segment per level. The panel only shows the state and reports what the user does; the Explode area
// (ExplodeArea.hpp) owns the state and the view.
#include <QWidget>

#include <string>
#include <utility>
#include <vector>

#include "opad/explode.hpp"

class PanelFooter;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QToolButton;

class ExplodePanel : public QWidget {
  Q_OBJECT
 public:
  explicit ExplodePanel(QWidget* parent = nullptr);
  // Shows the state without reporting it back. depth: the levels the root offers (explode_depth).
  void showSpec(const opad::ExplodeSpec& spec, int depth, bool on, bool lines);
  void showDistance(double t);
  void showPlaying(bool on);
  void showStatus(const QString& text);
  void showViews(const std::vector<std::pair<std::string, QString>>& views, const std::string& current);  // saved exploded views
  int dragAxis() const { return m_dragAxis; }  // 0: the part's own direction, 1-3: X, Y, Z
  PanelFooter* footer() const { return m_footer; }
  // benches
  const QList<QToolButton*>& levelButtons() const { return m_levels; }
  QSlider* slider() const { return m_slider; }
  QLineEdit* distanceBox() const { return m_distance; }
  QToolButton* playButton() const { return m_play; }
  QComboBox* modeBox() const { return m_mode; }
  QComboBox* viewBox() const { return m_views; }
  QCheckBox* switchBox() const { return m_switch; }
  QPushButton* updateButton() const { return m_update; }

 signals:
  void switched(bool on);
  void levelsChosen(int levels);  // 0: every level
  void distanceChosen(double t);  // the slider or a typed percentage
  void playRequested();           // Play / Pause, Space
  void modeChosen(const QString& mode, int axis);  // radial | axis | stack; axis 0-2: X, Y, Z, 3: the view's up
  void spacingChosen(double spacing);
  void stagesChosen(const QString& stages);  // levels | together | units
  void attachSmallToggled(bool on);
  void linesToggled(bool on);
  void dragAxisChosen(int axis);
  void resetRequested();
  void saveRequested();
  void updateRequested();
  void viewChosen(const std::string& id);

 protected:
  void keyPressEvent(QKeyEvent* e) override;

 private:
  void rebuildLevels(int depth);
  QCheckBox* m_switch;
  QLabel* m_status;
  QWidget* m_levelRow;
  QHBoxLayout* m_levelLayout;
  QList<QToolButton*> m_levels;
  int m_depth = -1;
  QToolButton* m_play;
  QSlider* m_slider;
  QLineEdit* m_distance;
  QComboBox* m_mode;
  QComboBox* m_axis;
  QDoubleSpinBox* m_spacing;
  QComboBox* m_stages;
  QCheckBox* m_attach;
  QCheckBox* m_lines;
  QList<QToolButton*> m_dragButtons;
  int m_dragAxis = 0;
  QComboBox* m_views;
  PanelFooter* m_footer;
  QPushButton* m_update;
};
