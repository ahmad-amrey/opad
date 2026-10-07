#pragma once
// A time-series chart for the Simulate workspace: one or more curves of a study run against time, a cursor at the frame
// shown, the value under it. Clicking or dragging picks a time (timePicked).
#include <QColor>
#include <QString>
#include <QWidget>
#include <vector>

class SimPlot : public QWidget {
  Q_OBJECT
 public:
  struct Curve {
    QString name, unit;
    std::vector<double> v;
  };
  explicit SimPlot(QWidget* parent = nullptr);
  void setData(std::vector<double> t, std::vector<Curve> curves);
  void setCursor(int frame);  // -1: none
  int curves() const { return int(m_curves.size()); }
  QSize sizeHint() const override { return {320, 170}; }
  QSize minimumSizeHint() const override { return {200, 120}; }
 signals:
  void framePicked(int frame);

 protected:
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;

 private:
  QRect plotRect() const;
  void pick(int x);
  std::vector<double> m_t;
  std::vector<Curve> m_curves;
  int m_cursor = -1;
};
