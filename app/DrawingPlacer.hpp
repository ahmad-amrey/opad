#pragma once
#include "Viewport.hpp"
#include "Panels.hpp"
#include "Jobs.hpp"
#include <QPointer>
class QLineEdit;
class QLabel;
class QPushButton;

// Import of a drawing with no face selected (TODO 10 A12): after the plane is chosen, the drawing is shown on it, its
// own origin at the plane's, and moved there before the import op is written: dragged, given a typed offset, or
// snapped by one of its vertices onto another vertex (a body's, a drawing's or a sketch's) projected on the plane. The
// result is one placement matrix, stored in the import op; a sketch converted later keeps that plane and origin.
class DrawingPlacer : public QObject {
  Q_OBJECT
 public:
  DrawingPlacer(AppDocument* doc, Viewport* view, JobRunner* jobs, QWidget* window);
  void start(const QString& file, const opad::Frame& plane, std::function<void(ToolPanel*)> open);
  void cancel();
  bool active() const { return m_active; }
  ToolPanel* panel() const { return m_panel; }
  opad::Mat4 placement() const;  // drawing XY -> world, with the current offset
  void setOffset(double u, double v);
  bool snap(const opad::Vec3& from, const opad::Vec3& to);  // move so `from` (on the drawing) lands on `to`
  std::function<void(const opad::Mat4&)> placed;  // Place: import with this placement
  std::function<void()> back;                     // choose another plane
 signals:
  void cancelled();
  void ready();  // the drawing is on screen and can be moved
 protected:
  bool eventFilter(QObject*, QEvent*) override;
 private:
  void stop();
  void refresh();
  void move();
  bool nearestVertex(const QPointF& at, opad::Vec3& world) const;  // of the drawing, within a few pixels
  AppDocument* m_doc;
  Viewport* m_view;
  JobRunner* m_jobs;
  ToolPanel* m_panel;
  QLabel* m_status;
  QLineEdit *m_u, *m_v;
  QPushButton *m_snap, *m_place;
  bool m_active = false, m_dragging = false, m_loaded = false;
  int m_snapStage = 0;  // 1: pick a vertex of the drawing, 2: pick where it goes
  opad::Vec3 m_snapFrom{};
  int m_serial = 0;
  QPointer<Job> m_job;
  QString m_file;
  opad::Frame m_plane;
  double m_du = 0, m_dv = 0, m_pressU = 0, m_pressV = 0, m_startU = 0, m_startV = 0;
  std::vector<gp_Pnt> m_vertices;  // drawing coordinates
  Handle(AIS_InteractiveObject) m_preview, m_marker;
  Viewport::SelFilter m_oldFilter = Viewport::SelFilter::Body;
};
