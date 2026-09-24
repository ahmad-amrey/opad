#pragma once
#include "Viewport.hpp"
#include <QLineEdit>

// A small, camera-aware scalar handle shared by sketch and solid tools.
class DimensionHandle : public QWidget {
  Q_OBJECT
 public:
  explicit DimensionHandle(Viewport* viewport);
  void configure(const opad::Vec3& origin,const opad::Vec3& axis,double value,const QString& expression);
  void reposition();
 signals:
  void valueChanged(const QString& expression);
 protected:
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  bool eventFilter(QObject*,QEvent*) override;
 private:
  Viewport* m_view;
  QLineEdit* m_edit;
  opad::Vec3 m_origin{},m_axis{1,0,0};
  QPointF m_start,m_screenAxis;
  double m_value=0,m_startValue=0;
  bool m_dragging=false;
};
