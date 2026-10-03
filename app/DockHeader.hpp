#pragma once
#include <QLabel>
#include <QSize>
#include <QWidget>

// ---------------------------------------------------------------- dock header (28 px: title 500 fg2, float/close 16 px)
class QDockWidget;
class DockHeader : public QWidget {
  Q_OBJECT
 public:
  DockHeader(const QString& title, QDockWidget* dock);
  void setTitle(const QString& t);
  // QDockWidget places its content below the title bar's *size hint*, so it must match the fixed 28 px.
  QSize sizeHint() const override { return QSize(QWidget::sizeHint().width(), 28); }
  QSize minimumSizeHint() const override { return QSize(0, 28); }
 private:
  QLabel* m_title;
};
