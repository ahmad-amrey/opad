#pragma once
#include <QLabel>
#include <QString>
#include <QWidget>

// ---------------------------------------------------------------- progress strip (bottom of the window)
// Shown in the status bar for any operation that takes time. Carries a phase bar and, for multi-phase
// work like opening a file, an overall bar too, plus a Cancel button.
class QProgressBar;
class QPushButton;
class ProgressStrip : public QWidget {
  Q_OBJECT
 public:
  explicit ProgressStrip(QWidget* parent = nullptr);
  void begin(const QString& title, bool twoBars);
  void setPhase(const QString& text, int percent);  // percent < 0: indeterminate
  void setOverall(int percent);
  void finish();
 signals:
  void cancelRequested();
 protected:
  void resizeEvent(QResizeEvent* e) override;
 private:
  void setTitle(const QString& text);
  QString m_fullTitle;
  QLabel* m_title;
  QProgressBar* m_phaseBar;
  QLabel* m_phasePct;
  QLabel* m_overallLabel;
  QProgressBar* m_overallBar;
  QLabel* m_overallPct;
  QPushButton* m_cancel;
};
