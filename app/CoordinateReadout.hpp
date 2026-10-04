#pragma once
// The cursor's coordinates in the status bar (UI-112, UI-90), live as the mouse moves over the view, in the shown unit
// and precision: in a sketch, the sketch's own X and Y; over a drawing, its X and Y as its file has them (the 2D area
// says which drawing, setDrawingPoint); on a body, where the mouse meets it (X Y Z); in 2D mode, the two axes of the view
// plane; elsewhere, where the mouse ray meets the XY plane. The point an object snap shows is taken exactly. A tag before
// the numbers says which. The numbers stay left to right in a right-to-left UI. Updates are coalesced (30 ms) and cost a
// plane intersection, the view's last detection or one ray into its picking structures, nothing that scales with the
// model.
#include <QLabel>
#include <QPointF>
#include <QTimer>
#include <functional>

#include "opad/scene.hpp"

class Viewport;

class CoordinateReadout : public QLabel {
  Q_OBJECT
 public:
  enum class Source { None, Model, Plane, Sketch, View, Drawing };
  // sketchFrame: the open sketch's frame (false: no sketch is open).
  CoordinateReadout(Viewport* view, std::function<bool(opad::Frame&)> sketchFrame, QWidget* parent = nullptr);
  // A drawing under the mouse at `pos` (or at `snapped`, the snap's world point, when one shows): true with the point in
  // the drawing's own coordinates and its name. Unset or false: the readout goes on as for a model.
  using DrawingPoint = std::function<bool(const QPointF& pos, const opad::Vec3* snapped, opad::Vec3& point, QString& name)>;
  void setDrawingPoint(DrawingPoint drawing) { m_drawing = std::move(drawing); }
  void updateAt(const QPointF& pos);  // the readout for the mouse at this point of the view
  Source source() const { return m_source; }
  // World (Model, Plane, View); the sketch's u, v, 0 (Sketch); the drawing's own x, y, 0 (Drawing).
  const opad::Vec3& point() const { return m_point; }
  bool snapped() const { return m_snapped; }  // the point is the one an object snap shows
 protected:
  bool eventFilter(QObject* o, QEvent* e) override;
 private:
  void show(Source source, const opad::Vec3& p, const QString& axes, bool snapped = false, const QString& name = {});
  Viewport* m_view;
  std::function<bool(opad::Frame&)> m_sketchFrame;
  DrawingPoint m_drawing;
  QTimer m_timer;
  QPointF m_pos;
  Source m_source = Source::None;
  opad::Vec3 m_point{0, 0, 0};
  bool m_snapped = false;
};
