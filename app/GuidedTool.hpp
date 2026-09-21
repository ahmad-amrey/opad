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
#include <QScrollArea>

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
  QString m_icon, m_title, m_hints;
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
  QSize preferredSize(int width);
 signals:
  void clearRequested();
  void pinRequested();
  void componentsChanged(bool on);
  void contentSizeChanged();
 protected:
  void resizeEvent(QResizeEvent* event) override;
  bool eventFilter(QObject* object, QEvent* event) override;
 private:
  void sizeResults(int width);
  QVBoxLayout* m_stepRows;
  QLabel *m_title, *m_subtitle, *m_state;
  QTreeWidget* m_grid = nullptr;
  QWidget* m_footer;
  QPushButton* m_pin;
  QCheckBox* m_components;
  QPushButton* m_copy;
  QScrollArea* m_scroll;
  QWidget* m_body;
  int m_nameWidth = 0, m_keyWidth = 80, m_valueWidth = 120;
};
