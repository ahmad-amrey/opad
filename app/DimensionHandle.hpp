#pragma once
#include "Viewport.hpp"
#include <QLineEdit>
#include <QHash>
#include <QPointer>
class JobRunner;
class Job;

// A small, camera-aware scalar handle shared by sketch and solid tools.
class DimensionHandle : public QWidget {
  Q_OBJECT
 public:
  explicit DimensionHandle(Viewport* viewport,JobRunner* jobs);
  ~DimensionHandle() override;
  void configure(const opad::Vec3& origin,const opad::Vec3& axis,double value,const QString& expression);
  void reposition();
  using Segment=std::array<opad::Vec3,2>;
  void setAnchorSegments(std::vector<Segment> segments);
  bool interacting() const {return m_dragging || m_edit->hasFocus();}
 signals:
  void valueChanged(const QString& expression);
 protected:
  void showEvent(QShowEvent*) override;
  void hideEvent(QHideEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  bool eventFilter(QObject*,QEvent*) override;
 private:
  void indexAnchors();
  JobRunner* m_jobs;
  QPointer<Job> m_indexJob;
  QHash<quint64,QVector<size_t>> m_cells;
  std::vector<std::array<QPointF,2>> m_screenSegments;
  bool m_indexReady=false;
  Viewport* m_view;
  QLineEdit* m_edit;
  opad::Vec3 m_origin{},m_axis{1,0,0};
  QPointF m_start,m_screenAxis;
  double m_value=0,m_startValue=0;
  bool m_dragging=false,m_drawn=false;
  QPointF m_arrowStart,m_arrowEnd;
  Handle(AIS_InteractiveObject) m_arrow;
  std::vector<Segment> m_segments;
};
