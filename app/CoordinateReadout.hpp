#pragma once
// The cursor's coordinates in the status bar (UI-112), live as the mouse moves over the view, in the shown unit and
// precision: on a body, where the mouse meets it (X Y Z); in a sketch, the sketch's own X and Y; in 2D mode, the two
// axes of the view plane; elsewhere, where the mouse ray meets the XY plane. A tag before the numbers says which. The
// numbers stay left to right in a right-to-left UI. Updates are coalesced (30 ms) and cost a plane intersection and the
// view's last detection, nothing that scales with the model.
#include <QLabel>
#include <QPointF>
#include <QTimer>
#include <functional>

#include "opad/scene.hpp"

class Viewport;

class CoordinateReadout : public QLabel {
  Q_OBJECT
 public:
  enum class Source { None, Model, Plane, Sketch, View };
  // sketchFrame: the open sketch's frame (false: no sketch is open).
  CoordinateReadout(Viewport* view, std::function<bool(opad::Frame&)> sketchFrame, QWidget* parent = nullptr);
  void updateAt(const QPointF& pos);  // the readout for the mouse at this point of the view
  Source source() const { return m_source; }
  const opad::Vec3& point() const { return m_point; }  // world (Model, Plane, View); the sketch's u, v, 0 (Sketch)
 protected:
  bool eventFilter(QObject* o, QEvent* e) override;
 private:
  void show(Source source, const opad::Vec3& p, const QString& axes);
  Viewport* m_view;
  std::function<bool(opad::Frame&)> m_sketchFrame;
  QTimer m_timer;
  QPointF m_pos;
  Source m_source = Source::None;
  opad::Vec3 m_point{0, 0, 0};
};
