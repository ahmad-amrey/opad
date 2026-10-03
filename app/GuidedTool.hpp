#pragma once
// Guided tools (handoff: "the interactive inspect flow"). A tool is a short list of steps, one pick each; the
// user starts the tool first and is walked through it: a prompt bar at the top centre of the viewport names the
// step that is waiting, the tool panel lists the steps above the result, Esc goes one step back. The flow itself
// (what a pick means, when the result is computed) lives in MainWindow; these are its two views.
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <QCheckBox>
#include <QComboBox>
#include <QScrollArea>

#include "PanelFooter.hpp"

class ToolGuide;

struct ToolStep {
  QString label;   // "Select first face": follows the active selection filter
  QString picked;  // the picked target, empty while the step is open
};

// 16 px numbered ring: pending fg3, waiting sel, done filled sel with a check.
enum class StepState { Pending, Waiting, Done };
QPixmap stepRing(StepState state, int number, qreal dpr);

// Top centre of the viewport, 32 px, bg2, 1 px sel border: icon + name, one chip per step, mono key hints.
class PromptBar : public QWidget {
  Q_OBJECT
 public:
  explicit PromptBar(QWidget* parent = nullptr);
  void set(const QString& icon, const QString& title, const QList<ToolStep>& steps, const QString& hints);
  QString hints() const { return m_hints; }
  const QList<ToolStep>& steps() const { return m_steps; }
  void setText(const QString& icon, const QString& title, const QString& text);  // a sentence instead of steps (a first-use hint)
  QSize sizeHint() const override;
 protected:
  void paintEvent(QPaintEvent*) override;
 private:
  struct Piece {
    enum Kind { Icon, Title, Rule, Ring, Text, Arrow, Hint } kind;
    QString text;
    StepState state = StepState::Pending;
    int number = 0;
    int width = 0;
  };
  QList<Piece> pieces() const;
  QString m_icon, m_title, m_hints, m_text;
  QList<ToolStep> m_steps;
};

// Content of the tool's floating panel: step list, summary, result grid, and the footer once every step is done.
class ToolStepsPanel : public QWidget {
  Q_OBJECT
 public:
  explicit ToolStepsPanel(QWidget* parent = nullptr);
  void setSteps(const QList<ToolStep>& steps, const QString& hover);  // hover: candidate under the mouse, shown in the waiting row
  void setSummary(const QString& title, const QString& subtitle, const QString& state);
  void setResult(const QList<QPair<QString, QString>>& rows);  // empty: nothing to show yet
  void setFooter(bool visible, bool canPin);
  void setComponentsState(bool visible, bool checked);
  void setAnchorOptions(const QStringList& labels, int current);
  // UI-107: the command's animated guide above the steps, looping the waiting step (made on first use).
  void setGuide(const QString& command);
  ToolGuide* guide() const { return m_guide; }
  QSize preferredSize(int width);
  int stepsHeight(int width) const;  // what the scrolled part needs at that width, new step rows counted at once
  PanelFooter* footer() const { return m_footer; }
 signals:
  void clearRequested();
  void pinRequested();
  void componentsChanged(bool on);
  void anchorChanged(int index);
  void contentSizeChanged();
 protected:
  void resizeEvent(QResizeEvent* event) override;
  bool eventFilter(QObject* object, QEvent* event) override;
 private:
  void sizeResults(int width);
  QVBoxLayout* m_stepRows;
  QLabel *m_title, *m_subtitle, *m_state;
  QTreeWidget* m_grid = nullptr;
  PanelFooter* m_footer;
  QCheckBox* m_components;
  QWidget* m_anchorRow;
  QComboBox* m_anchors;
  QPushButton* m_copy;
  QScrollArea* m_scroll;
  QWidget* m_body;
  ToolGuide* m_guide = nullptr;
  int m_nameWidth = 0, m_keyWidth = 80, m_valueWidth = 120;
};
