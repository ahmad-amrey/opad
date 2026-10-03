#pragma once
#include <QPoint>
#include <QRect>
#include <QTimer>
#include <QWidget>

// ---------------------------------------------------------------- loading shade
// While a file loads, a frameless translucent tool window (owned by the main window, so it stays above it and
// hides with it) covers the workspace: docks and the OpenGL viewport alike, which a child widget could not
// do. It darkens everything, shows a spinner and swallows mouse input; shortcuts are held back by MainWindow.
class LoadShade : public QWidget {
  Q_OBJECT
 public:
  explicit LoadShade(QWidget* owner);
  void place(const QRect& globalArea, const QPoint& spinnerCentreGlobal);  // cover this screen area, spinner here
 protected:
  void paintEvent(QPaintEvent*) override;
  void showEvent(QShowEvent*) override;
  void hideEvent(QHideEvent*) override;
 private:
  QTimer m_timer;
  int m_angle = 0;
  QPoint m_spinner;  // local
};
