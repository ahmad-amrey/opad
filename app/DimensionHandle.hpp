#pragma once
#include "Viewport.hpp"
#include <QLineEdit>
#include <QHash>
#include <QPointer>
class JobRunner;
class Job;
class QLabel;

// A camera-aware scalar handle shared by sketch and solid tools: a flat arrow that always faces the camera (drawn in
// the selection colour, pulled to change the value) and a value box beside it.
class DimensionHandle : public QWidget {
  Q_OBJECT
 public:
  explicit DimensionHandle(Viewport* viewport,JobRunner* jobs);
  ~DimensionHandle() override;
  void configure(const opad::Vec3& origin,const opad::Vec3& axis,double value,const QString& expression);
  void setLabel(const QString& label);
  void reposition();
  using Segment=std::array<opad::Vec3,2>;
  void setAnchorSegments(std::vector<Segment> segments);
  bool interacting() const {return m_dragging || m_edit->hasFocus();}
  bool dragging() const {return m_dragging;}
  double value() const {return m_value;}  // what the drag or the arrows made of it (the box shows it rounded)
  // How far the arrow sits along the axis per unit of value: 0.5 for a symmetric extrusion, whose end moves half the
  // distance. Kept while a drag runs.
  void setScale(double scale) {if(!m_dragging)m_scale=scale;}
 signals:
  void valueChanged(const QString& expression);
  void accepted();  // Enter in the value box: apply the operation
 protected:
  void showEvent(QShowEvent*) override;
  void hideEvent(QHideEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void wheelEvent(QWheelEvent*) override;
  bool eventFilter(QObject*,QEvent*) override;
 private:
  void indexAnchors();
  void restyle();
  void fit();
  void nudge(double steps,Qt::KeyboardModifiers modifiers);
  void setText(const QString& text,bool notify);
  JobRunner* m_jobs;
  QPointer<Job> m_indexJob;
  QHash<quint64,QVector<size_t>> m_cells;
  std::vector<std::array<QPointF,2>> m_screenSegments;
  bool m_indexReady=false;
  Viewport* m_view;
  QLabel* m_label;
  QLineEdit* m_edit;
  QLabel* m_result;
  QString m_before;  // the text when the box took focus: Esc restores it
  opad::Vec3 m_origin{},m_axis{1,0,0};
  QPointF m_start,m_screenAxis;
  double m_value=0,m_startValue=0,m_scale=1;
  bool m_dragging=false,m_drawn=false;
  QPointF m_arrowStart,m_arrowEnd;
  Handle(AIS_InteractiveObject) m_arrow;
  std::vector<Segment> m_segments;
};
